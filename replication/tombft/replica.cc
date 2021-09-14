#include "replication/tombft/replica.h"

#include "lib/assert.h"
#include "replication/tombft/message.h"
#include "replication/tombft/tombft-proto.pb.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace tombft {

TomBFTReplica::TomBFTReplica(const Configuration &config, int myIdx,
                             bool initialize, Transport *transport,
                             const Security &security, AppReplica *app)
    : Replica(config, 0, myIdx, initialize, transport, app),
      security(security),
      vs(0, 0),
      log(false),
      prepare_set(2 * configuration.f),
      commit_set(2 * configuration.f + 1) {
  transport->ListenOnMulticast(this, config);

  query_timer = new Timeout(transport, 800, [this]() {
    if (configuration.GetLeaderIndex(vs.view) == replicaIdx) {
      SendGapFindMessage(vs.opnum + 1);
      return;
    }

    RWarning("Query opnum = %lu", vs.opnum + 1);
    proto::Message m;
    auto &query = *m.mutable_query();
    query.set_view(vs.view);
    query.set_opnum(vs.opnum + 1);
    query.set_replicaid(replicaIdx);
    query.set_sig(string());
    string sig;
    this->security.ReplicaSigner(replicaIdx)
        .Sign(query.SerializeAsString(), sig);
    query.set_sig(sig);

    this->transport->SendMessageToReplica(
        this, configuration.GetLeaderIndex(vs.view), TomBFTMessage(m));
  });
}

void TomBFTReplica::ReceiveMessage(const TransportAddress &remote, void *buf,
                                   size_t size) {
  proto::Message msg;
  TomBFTMessage m(msg);
  m.Parse(buf, size);
  switch (msg.msg_case()) {
    case proto::Message::kRequest:
      HandleRequest(remote, msg, m.meta);
      break;
    case proto::Message::kQuery:
      HandleQuery(remote, *msg.mutable_query());
      break;
    case proto::Message::kQueryReply:
      HandleQueryReply(remote, *msg.mutable_query_reply());
      break;
    case proto::Message::kGapFindMessage:
      HandleGapFindMessage(remote, *msg.mutable_gap_find_message());
      break;
    case proto::Message::kGapRecvMessage:
      HandleGapRecvMessage(remote, *msg.mutable_gap_recv_message());
      break;
    case proto::Message::kGapDecision:
      HandleGapDecision(remote, *msg.mutable_gap_decision());
      break;
    case proto::Message::kGapPrepare:
      HandleGapPrepare(remote, *msg.mutable_gap_prepare());
      break;
    case proto::Message::kGapCommit:
      HandleGapCommit(remote, *msg.mutable_gap_commit());
      break;
    default:
      Panic("Received unexpected message type #%u", msg.msg_case());
  }
}

void TomBFTReplica::HandleRequest(const TransportAddress &remote,
                                  proto::Message &m,
                                  TomBFTMessage::Header &meta) {
  Assert(meta.sess_num != 0);
  std::string seq_sig(meta.sig_list[replicaIdx].hmac, HMAC_LENGTH);
  if (!security.SequencerVerifier(replicaIdx)
           .Verify(m.SerializeAsString(), seq_sig)) {
    RWarning("Incorrect sequencer signature");
    return;
  }
  if (!security.ClientVerifier().Verify(m.request().req().SerializeAsString(),
                                        m.request().sig())) {
    RWarning("Incorrect client signature");
    return;
  }

  if (meta.msg_num == vs.msgnum + 1) {
    vs.msgnum += 1;
    vs.opnum += 1;
    log.Append(new TomBFTLogEntry(vs, LOG_STATE_EXECUTED, m, meta));

    proto::Message reply_msg;
    reply_msg.mutable_reply()->set_view(vs.view);
    reply_msg.mutable_reply()->set_replicaid(replicaIdx);
    reply_msg.mutable_reply()->set_opnum(vs.opnum);
    reply_msg.mutable_reply()->set_clientreqid(m.request().req().clientreqid());

    Execute(vs.opnum, m.request().req(), *reply_msg.mutable_reply());

    reply_msg.mutable_reply()->set_sig(string());
    security.ReplicaSigner(replicaIdx)
        .Sign(reply_msg.reply().SerializeAsString(),
              *reply_msg.mutable_reply()->mutable_sig());

    TransportAddress *client_addr = transport->LookupAddress(
        ReplicaAddress(m.request().req().clientaddr()));
    transport->SendMessage(this, *client_addr, TomBFTMessage(reply_msg));
    return;
  }

  pending_request_message[meta.msg_num] = m;
  pending_request_meta[meta.msg_num] = meta;
  RDebug("slow path msg = %lu", meta.msg_num);
  ProcessPendingRequest();
}

void TomBFTReplica::ProcessPendingRequest() {
  if (!pending_request_message.size()) return;
  auto &m = pending_request_message.begin()->second;
  auto &meta = pending_request_meta.begin()->second;

  if (vs.msgnum == 0) {
    RNotice("Start msgnum = %lu", meta.msg_num);
    vs.msgnum = meta.msg_num - 1;  // Jialin's hack
  }
  if (meta.msg_num <= vs.msgnum) {
    RWarning("Receive duplicated sequencing Request msgnum = %lu",
             meta.msg_num);
    pending_request_message.erase(pending_request_message.begin());
    pending_request_meta.erase(pending_request_meta.begin());
    ProcessPendingRequest();
    return;
  }
  if (meta.msg_num != vs.msgnum + 1) {
    if (!query_timer->Active()) {
      RNotice("Sched gap query msgnum = %lu opnum = %lu", vs.msgnum + 1,
              vs.opnum + 1);
      query_timer->Start();
    }
    return;
  } else {
    if (query_timer->Active()) {
      RNotice("Gap messag received");
    }
    query_timer->Stop();
  }

  vs.msgnum += 1;
  vs.opnum += 1;
  log.Append(new TomBFTLogEntry(vs, LOG_STATE_EXECUTED, m, meta));

  proto::Message reply_msg;
  reply_msg.mutable_reply()->set_view(vs.view);
  reply_msg.mutable_reply()->set_replicaid(replicaIdx);
  reply_msg.mutable_reply()->set_opnum(vs.opnum);
  reply_msg.mutable_reply()->set_clientreqid(m.request().req().clientreqid());

  Execute(vs.opnum, m.request().req(), *reply_msg.mutable_reply());

  reply_msg.mutable_reply()->set_sig(string());
  security.ReplicaSigner(replicaIdx)
      .Sign(reply_msg.reply().SerializeAsString(),
            *reply_msg.mutable_reply()->mutable_sig());

  TransportAddress *client_addr =
      transport->LookupAddress(ReplicaAddress(m.request().req().clientaddr()));
  transport->SendMessage(this, *client_addr, TomBFTMessage(reply_msg));

  pending_request_message.erase(pending_request_message.begin());
  pending_request_meta.erase(pending_request_meta.begin());
  ProcessPendingRequest();
}

void TomBFTReplica::HandleQuery(const TransportAddress &remote,
                                proto::Query &msg) {
  if (configuration.GetLeaderIndex(vs.view) != replicaIdx) {
    RWarning("Unexpected Query");
    return;
  }
  string sig = msg.sig();
  msg.set_sig(string());
  if (!security.ReplicaVerifier(msg.replicaid())
           .Verify(msg.SerializeAsString(), sig)) {
    RWarning("Incorrect Query signature");
    return;
  }

  RNotice("Reply to Query opnum = %lu", msg.opnum());
  // TODO find by message number
  auto *e = log.Find(msg.opnum());
  if (!e) {
    SendGapFindMessage(msg.opnum());
    return;
  }
  auto &entry = e->As<TomBFTLogEntry>();
  proto::Message m;
  auto &query_reply = *m.mutable_query_reply();
  query_reply.set_view(vs.view);
  query_reply.set_opnum(msg.opnum());
  query_reply.set_msgnum(entry.meta.msg_num);
  *query_reply.mutable_req() = entry.req_msg;
  for (int i = 0; i < 4; i += 1) {
    query_reply.add_hmac_vec(entry.meta.sig_list[i].hmac, HMAC_LENGTH);
  }
  query_reply.set_replicaid(replicaIdx);
  query_reply.set_sig(string());
  // string sig;
  security.ReplicaSigner(replicaIdx).Sign(query_reply.SerializeAsString(), sig);
  query_reply.set_sig(sig);
  transport->SendMessage(this, remote, TomBFTMessage(m));
}

void TomBFTReplica::HandleQueryReply(const TransportAddress &remote,
                                     proto::QueryReply &msg) {
  string sig = msg.sig();
  msg.set_sig(string());
  if (!security.ReplicaVerifier(replicaIdx)
           .Verify(msg.SerializeAsString(), sig)) {
    RWarning("Incorrect QueryReply signature");
    return;
  }
  // ad-hoc disable
  // std::string seq_sig(msg.hmac_vec(replicaIdx), HMAC_LENGTH);
  // if (!security.SequencerVerifier(replicaIdx)
  //          .Verify(msg.req().SerializeAsString(), seq_sig)) {
  //   RWarning("Incorrect sequence in QueryReply");
  //   return;
  // }
  if (!security.ClientVerifier().Verify(
          msg.req().request().req().SerializeAsString(),
          msg.req().request().sig())) {
    RWarning("Incorrect client signature in QueryReply");
    return;
  }

  pending_request_message[msg.msgnum()] = msg.req();
  TomBFTMessage::Header meta;
  meta.msg_num = msg.msgnum();
  for (int i = 0; i < 4; i += 1) {
    memcpy(meta.sig_list[i].hmac, msg.hmac_vec(i).c_str(), HMAC_LENGTH);
  }
  pending_request_meta[msg.msgnum()] = meta;
  RNotice("Gap opnum = %lu filled", msg.opnum());
  ProcessPendingRequest();
}

void TomBFTReplica::SendGapFindMessage(opnum_t opnum) {
  RWarning("Gap find message opnum = %lu", opnum);
  proto::Message m;
  auto &gap_find_message = *m.mutable_gap_find_message();
  gap_find_message.set_view(vs.view);
  gap_find_message.set_opnum(opnum);
  gap_find_message.set_replicaid(replicaIdx);
  // TODO sig
  gap_find_message.set_sig(string());
  transport->SendMessageToAll(this, TomBFTMessage(m));
}

void TomBFTReplica::HandleGapFindMessage(const TransportAddress &remote,
                                         proto::GapFindMessage &msg) {
  // TODO sig
  auto e = log.Find(msg.opnum());
  if (!e) {
    // TODO gap drop
    return;
  }
  auto &entry = e->As<TomBFTLogEntry>();
  RNotice("GapRecv opnum = %lu msgnum = %lu", entry.viewstamp.opnum,
          entry.meta.msg_num);
  proto::Message m;
  auto &gap_recv_message = *m.mutable_gap_recv_message();
  gap_recv_message.set_view(vs.view);
  gap_recv_message.set_opnum(msg.opnum());
  gap_recv_message.set_msgnum(entry.meta.msg_num);
  *gap_recv_message.mutable_req() = entry.req_msg;
  for (int i = 0; i < 4; i += 1) {
    gap_recv_message.add_hmac_vec(entry.meta.sig_list[i].hmac, HMAC_LENGTH);
  }
  gap_recv_message.set_replicaid(replicaIdx);
  // TODO sig
  gap_recv_message.set_sig(string());
  // gap_recv_message.clear_sig();
  // // string sig;
  // security.ReplicaSigner(replicaIdx).Sign(gap_recv_message.SerializeAsString(),
  // sig); gap_recv_message.set_sig(sig);
  transport->SendMessage(this, remote, TomBFTMessage(m));
}

void TomBFTReplica::HandleGapRecvMessage(const TransportAddress &remote,
                                         proto::GapRecvMessage &msg) {
  if (replicaIdx != configuration.GetLeaderIndex(vs.view)) {
    RPanic("Unexpected GapRecv");
  }
  proto::Message m;
  auto &gap_decision = *m.mutable_gap_decision();
  gap_decision.set_view(vs.view);
  gap_decision.set_opnum(msg.opnum());
  *gap_decision.mutable_gap_recv_message() = msg;
  gap_decision.set_replicaid(replicaIdx);
  // TODO sig
  gap_decision.set_sig(string());
  decision_map[msg.opnum()] = gap_decision;
  transport->SendMessageToAll(this, TomBFTMessage(m));
}

void TomBFTReplica::HandleGapDecision(const TransportAddress &remote,
                                      proto::GapDecision &msg) {
  decision_map[msg.opnum()] = msg;

  proto::Message m;
  auto &gap_prepare = *m.mutable_gap_prepare();
  gap_prepare.set_view(vs.view);
  gap_prepare.set_opnum(msg.opnum());
  gap_prepare.set_recv(true);  // TODO
  gap_prepare.set_replicaid(replicaIdx);
  // TODO sig
  gap_prepare.set_sig(string());
  transport->SendMessageToAll(this, TomBFTMessage(m));
  HandleGapPrepare(this->GetAddress(), gap_prepare);
}

void TomBFTReplica::HandleGapPrepare(const TransportAddress &remote,
                                     proto::GapPrepare &msg) {
  int replicaid = msg.replicaid();
  msg.set_replicaid(0);
  if (!prepare_set.Add(msg.opnum(), replicaid, msg)) {
    return;
  }

  proto::Message m;
  auto &gap_commit = *m.mutable_gap_commit();
  gap_commit.set_view(vs.view);
  gap_commit.set_opnum(msg.opnum());
  gap_commit.set_recv(msg.recv());
  gap_commit.set_replicaid(replicaIdx);
  // TODO sig
  gap_commit.set_sig(string());

  // if (past_prepared.count(msg.opnum())) {
  //   transport->SendMessage(this, remote, TomBFTMessage(m));
  // } else {
  transport->SendMessageToAll(this, TomBFTMessage(m));
  HandleGapCommit(this->GetAddress(), gap_commit);
  // }
  past_prepared.insert(msg.opnum());
}

void TomBFTReplica::HandleGapCommit(const TransportAddress &remote,
                                    proto::GapCommit &msg) {
  int replicaid = msg.replicaid();
  msg.set_replicaid(0);
  if (!commit_set.Add(msg.opnum(), replicaid, msg)) {
    return;
  }
  if (!decision_map.count(msg.opnum())) {
    // TODO
    return;  // for now this is the super slow path: resend query
  }
  auto &recv = decision_map[msg.opnum()].gap_recv_message();
  if (recv.msgnum() <= vs.msgnum) {
    RDebug("skip past msg = %lu", recv.msgnum());
    return;
  }

  pending_request_message[recv.msgnum()] = recv.req();
  TomBFTMessage::Header meta;
  meta.msg_num = recv.msgnum();
  for (int i = 0; i < 4; i += 1) {
    memcpy(meta.sig_list[i].hmac, recv.hmac_vec(i).c_str(), HMAC_LENGTH);
  }
  pending_request_meta[recv.msgnum()] = meta;
  ProcessPendingRequest();
}

}  // namespace tombft
}  // namespace dsnet