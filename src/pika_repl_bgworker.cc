// Copyright (c) 2019-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "include/pika_repl_bgworker.h"

#include <glog/logging.h>

#include "include/pika_conf.h"
#include "include/pika_cmd_table_manager.h"
#include "include/pika_server.h"

extern PikaServer* g_pika_server;
extern PikaConf* g_pika_conf;
extern PikaCmdTableManager* g_pika_cmd_table_manager;

PikaReplBgWorker::PikaReplBgWorker(int queue_size)
    : bg_thread_(queue_size) {
  bg_thread_.set_thread_name("ReplBgWorker");
  pink::RedisParserSettings settings;
  settings.DealMessage = &(PikaReplBgWorker::HandleWriteBinlog);
  redis_parser_.RedisParserInit(REDIS_PARSER_REQUEST, settings);
  redis_parser_.data = this;
}

int PikaReplBgWorker::StartThread() {
  return bg_thread_.StartThread();
}

void PikaReplBgWorker::ScheduleRequest(const std::shared_ptr<InnerMessage::InnerRequest> req,
    std::shared_ptr<pink::PbConn> conn, void* req_private_data) {
  ReplBgWorkerArg* arg = new ReplBgWorkerArg(req, conn, req_private_data, this);
  switch (req->type()) {
    case InnerMessage::kMetaSync:
      bg_thread_.Schedule(&PikaReplBgWorker::HandleMetaSyncRequest, static_cast<void*>(arg));
      break;
    case InnerMessage::kTrySync:
      break;
    case InnerMessage::kBinlogSync:
      bg_thread_.Schedule(&PikaReplBgWorker::HandleBinlogSyncRequest, static_cast<void*>(arg));
      break;
    case InnerMessage::kDbSync:
      break;
    default:
      break;
  }
}

void PikaReplBgWorker::ScheduleWriteDb(PikaCmdArgsType* argv, BinlogItem* binlog_item) {
  WriteDbBgArg* arg = new WriteDbBgArg(argv, binlog_item);
  bg_thread_.Schedule(&PikaReplBgWorker::HandleWriteDb, static_cast<void*>(arg));
}

void PikaReplBgWorker::HandleMetaSyncRequest(void* arg) {
  ReplBgWorkerArg* bg_worker_arg = static_cast<ReplBgWorkerArg*>(arg);
  const std::shared_ptr<InnerMessage::InnerRequest> req = bg_worker_arg->req;
  std::shared_ptr<pink::PbConn> conn = bg_worker_arg->conn;

  std::vector<TableStruct> table_structs = g_pika_conf->table_structs();
  InnerMessage::InnerResponse response;
  response.set_code(InnerMessage::kOk);
  response.set_type(InnerMessage::kMetaSync);
  InnerMessage::InnerResponse_MetaSync* meta_sync = response.mutable_meta_sync();
  meta_sync->set_classic_mode(g_pika_conf->classic_mode());
  for (const auto& table_struct : table_structs) {
    InnerMessage::InnerResponse_MetaSync_TableInfo* table_info = meta_sync->add_tables_info();
    table_info->set_table_name(table_struct.table_name);
    table_info->set_partition_num(table_struct.partition_num);
  }

  std::string reply_str;
  if (!response.SerializeToString(&reply_str)) {
    LOG(WARNING) << "Process MetaSync request serialization failed";
    delete bg_worker_arg;
    return;
  }
  int res = conn->WriteResp(reply_str);
  if (res) {
    LOG(WARNING) << "Process MetaSync request write resp failed";
    delete bg_worker_arg;
    return;
  }
  conn->NotifyWrite();
  delete bg_worker_arg;
}

void PikaReplBgWorker::HandleBinlogSyncRequest(void* arg) {
  ReplBgWorkerArg* bg_worker_arg = static_cast<ReplBgWorkerArg*>(arg);
  const std::shared_ptr<InnerMessage::InnerRequest> req = bg_worker_arg->req;
  std::shared_ptr<pink::PbConn> conn = bg_worker_arg->conn;
  std::vector<int>* index = static_cast<std::vector<int>* >(bg_worker_arg->req_private_data);
  PikaReplBgWorker* worker = bg_worker_arg->worker;
  worker->ip_port_ = conn->ip_port();
  for (size_t i = 0; i < index->size(); ++i) {
    const InnerMessage::InnerRequest::BinlogSync& binlog_req = req->binlog_sync((*index)[i]);
    if (!PikaBinlogTransverter::BinlogItemWithoutContentDecode(TypeFirst, binlog_req.binlog(), &worker->binlog_item_)) {
      LOG(WARNING) << "Binlog item decode failed";
      delete index;
      delete bg_worker_arg;
      return;
    }
    const char* redis_parser_start = binlog_req.binlog().data() + BINLOG_ENCODE_LEN;
    int redis_parser_len = static_cast<int>(binlog_req.binlog().size()) - BINLOG_ENCODE_LEN;
    int processed_len = 0;
    pink::RedisParserStatus ret = worker->redis_parser_.ProcessInputBuffer(
      redis_parser_start, redis_parser_len, &processed_len);
    if (ret != pink::kRedisParserDone) {
      LOG(WARNING) << "Redis parser failed";
      delete index;
      delete bg_worker_arg;
      return;
    }
  }

  // build response
  const InnerMessage::InnerRequest::BinlogSync& binlog_req =
    req->binlog_sync((*index)[(*index).size() - 1]);
  std::string table_name = binlog_req.table_name();
  uint32_t partition_id = binlog_req.partition_id();
  std::shared_ptr<Partition> partition = g_pika_server->GetTablePartitionById(table_name, partition_id);
  std::shared_ptr<Binlog> logger = partition->logger();
  uint32_t file_num;
  uint64_t offset;
  logger->GetProducerStatus(&file_num, &offset);

  InnerMessage::InnerResponse response;
  response.set_code(InnerMessage::kOk);
  response.set_type(InnerMessage::kBinlogSync);
  InnerMessage::InnerResponse_BinlogSync* binlog_sync_resp = response.mutable_binlog_sync();
  binlog_sync_resp->set_table_name(table_name);
  binlog_sync_resp->set_partition_id(partition_id);
  InnerMessage::SyncOffset* sync_offset = binlog_sync_resp->mutable_sync_offset();
  sync_offset->set_filenum(file_num);
  sync_offset->set_offset(offset);

  std::string reply_str;
  if (!response.SerializeToString(&reply_str)) {
    LOG(WARNING) << "Process MetaSync request serialization failed";
    delete index;
    delete bg_worker_arg;
    return;
  }

  int res = conn->WriteResp(reply_str);
  if (res) {
    LOG(WARNING) << "Process BinlogSync request write resp failed";
    delete index;
    delete bg_worker_arg;
    return;
  }
  conn->NotifyWrite();
  delete index;
  delete bg_worker_arg;
}

int PikaReplBgWorker::HandleWriteBinlog(pink::RedisParser* parser, const pink::RedisCmdArgsType& argv) {
  PikaReplBgWorker* worker = static_cast<PikaReplBgWorker*>(parser->data);
  const BinlogItem& binlog_item = worker->binlog_item_;
  g_pika_server->UpdateQueryNumAndExecCountTable(argv[0]);

  // Monitor related
  std::string monitor_message;
  if (g_pika_server->HasMonitorClients()) {
    std::string monitor_message = std::to_string(1.0 * slash::NowMicros() / 1000000)
      + " [" + worker->ip_port_ + "]";
    for (const auto& item : argv) {
      monitor_message += " " + slash::ToRead(item);
    }
    g_pika_server->AddMonitorMessage(monitor_message);
  }

  std::string opt = argv[0];
  Cmd* c_ptr = g_pika_cmd_table_manager->GetCmd(slash::StringToLower(opt));
  c_ptr->Initial(argv, g_pika_conf->default_table());

  g_pika_server->logger_->Lock();
  g_pika_server->logger_->Put(c_ptr->ToBinlog(binlog_item.exec_time(),
                                              std::to_string(binlog_item.server_id()),
                                              binlog_item.logic_id(),
                                              binlog_item.filenum(),
                                              binlog_item.offset()));
  g_pika_server->logger_->Unlock();

  PikaCmdArgsType *v = new PikaCmdArgsType(argv);
  BinlogItem *b = new BinlogItem(binlog_item);
  std::string dispatch_key = argv.size() >= 2 ? argv[1] : argv[0];
  g_pika_server->ScheduleReplDbTask(dispatch_key, v, b);
  return 0;
}

void PikaReplBgWorker::HandleWriteDb(void* arg) {
  WriteDbBgArg *bg_worker_arg = static_cast<WriteDbBgArg*>(arg);
  PikaCmdArgsType argv = *(bg_worker_arg->argv);
  BinlogItem binlog_item = *(bg_worker_arg->binlog_item);
  std::string opt = argv[0];
  slash::StringToLower(opt);

  // Get command
  Cmd* c_ptr = g_pika_cmd_table_manager->GetCmd(slash::StringToLower(opt));
  if (!c_ptr) {
    LOG(WARNING) << "Error operation from binlog: " << opt;
    delete bg_worker_arg;
    return;
  }
  c_ptr->res().clear();

  // Initial
  c_ptr->Initial(argv, g_pika_conf->default_table());
  if (!c_ptr->res().ok()) {
    LOG(WARNING) << "Fail to initial command from binlog: " << opt;
    delete bg_worker_arg;
    return;
  }

  uint64_t start_us = 0;
  if (g_pika_conf->slowlog_slower_than() >= 0) {
    start_us = slash::NowMicros();
  }

  // Add read lock for no suspend command
  if (!c_ptr->is_suspend()) {
    g_pika_server->RWLockReader();
  }

  c_ptr->Execute();

  if (!c_ptr->is_suspend()) {
    g_pika_server->RWUnlock();
  }

  if (g_pika_conf->slowlog_slower_than() >= 0) {
    int64_t duration = slash::NowMicros() - start_us;
    if (duration > g_pika_conf->slowlog_slower_than()) {
      LOG(ERROR) << "command: " << opt << ", start_time(s): " << start_us / 1000000 << ", duration(us): " << duration;
    }
  }

  delete bg_worker_arg;
}
