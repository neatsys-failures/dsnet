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
  // TODO
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

  if (meta.msg_num != vs.msgnum + 1) {
    NOT_IMPLEMENTED();  // slow path
  }

  vs.msgnum = vs.opnum = meta.msg_num;
  // TODO
  log.Append(new TomBFTLogEntry(vs, LOG_STATE_EXECUTED, m.request().req(), m));

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
}

}  // namespace tombft
}  // namespace dsnet