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
      log(false) {
  transport->ListenOnMulticast(this, config);

  query_timer = new Timeout(transport, 1000, [this]() { NOT_IMPLEMENTED(); });
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

  pending_request_message[meta.msg_num] = m;
  pending_request_meta[meta.msg_num] = meta;
  ProcessPendingRequest();
}

void TomBFTReplica::ProcessPendingRequest() {
  if (!pending_request_message.size()) return;
  auto &m = pending_request_message.begin()->second;
  auto &meta = pending_request_meta.begin()->second;

  if (vs.msgnum == 0) {
    vs.msgnum = meta.msg_num - 1;  // Jialin's hack
  }
  if (meta.msg_num != vs.msgnum + 1) {
    if (!query_timer->Active()) {
      query_timer->Start();
    }
    return;
  } else {
    query_timer->Stop();
  }

  vs.msgnum += 1;
  vs.opnum += 1;
  // TODO
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
  if (!security.ReplicaVerifier(msg.replicaid())
           .Verify(msg.SerializeAsString(), msg.sig())) {
    RWarning("Incorrect Query signature");
    return;
  }
  // TODO find by message number
  auto *e = log.Find(msg.opnum());
  if (!e) {
    // TODO gap
    NOT_IMPLEMENTED();
  }
  auto &entry = e->As<TomBFTLogEntry>();
  proto::Message m;
  auto &query_reply = *m.mutable_query_reply();
  query_reply.set_view(vs.view);
  query_reply.set_opnum(msg.opnum());
  *query_reply.mutable_req() = entry.req_msg;
  for (int i = 0; i < 4; i += 1) {
    query_reply.add_hmac_vec(entry.meta.sig_list[i].hmac);
  }
  query_reply.set_replicaid(replicaIdx);
  query_reply.clear_sig();
  string sig;
  security.ReplicaSigner(replicaIdx).Sign(query_reply.SerializeAsString(), sig);
  query_reply.set_sig(sig);
  transport->SendMessage(this, remote, TomBFTMessage(m));
}

}  // namespace tombft
}  // namespace dsnet