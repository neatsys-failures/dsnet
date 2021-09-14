//
#pragma once

#include "common/log.h"
#include "common/quorumset.h"
#include "common/replica.h"
#include "lib/signature.h"
#include "replication/tombft/message.h"
#include "replication/tombft/tombft-proto.pb.h"

namespace dsnet {
namespace tombft {

struct TomBFTLogEntry : public LogEntry {
  proto::Message req_msg;
  TomBFTMessage::Header meta;

  TomBFTLogEntry(viewstamp_t vs, LogEntryState state,
                 const proto::Message &req_msg,
                 const TomBFTMessage::Header &meta)
      : LogEntry(vs, state, req_msg.request().req()),
        req_msg(req_msg),
        meta(meta) {}
};

class TomBFTReplica : public Replica {
 public:
  TomBFTReplica(const Configuration &config, int myIdx, bool initialize,
                Transport *transport, const Security &securtiy,
                AppReplica *app);
  ~TomBFTReplica() {}

  void ReceiveMessage(const TransportAddress &remote, void *buf,
                      size_t size) override;

 private:
  const Security &security;
  Timeout *query_timer;
  viewstamp_t vs;
  Log log;

  void HandleRequest(const TransportAddress &remote, proto::Message &m,
                     TomBFTMessage::Header &meta);
  std::map<uint64_t, proto::Message> pending_request_message;
  std::map<uint64_t, TomBFTMessage::Header> pending_request_meta;

  void HandleQuery(const TransportAddress &remote, proto::Query &msg);
  void HandleQueryReply(const TransportAddress &remote, proto::QueryReply &msg);

  void ProcessPendingRequest();

  void SendGapFindMessage(opnum_t opnum);
  void HandleGapFindMessage(const TransportAddress &remote,
                            proto::GapFindMessage &msg);
  void HandleGapRecvMessage(const TransportAddress &remote,
                            proto::GapRecvMessage &msg);

  std::unordered_map<opnum_t, proto::GapDecision> decision_map;
  void HandleGapDecision(const TransportAddress &remote,
                         proto::GapDecision &msg);

  ByzantineProtoQuorumSet<opnum_t, proto::GapPrepare> prepare_set;
  std::unordered_set<opnum_t> past_prepared;
  void HandleGapPrepare(const TransportAddress &remote, proto::GapPrepare &msg);

  ByzantineProtoQuorumSet<opnum_t, proto::GapCommit> commit_set;
  void HandleGapCommit(const TransportAddress &remote, proto::GapCommit &msg);
};

}  // namespace tombft
}  // namespace dsnet
