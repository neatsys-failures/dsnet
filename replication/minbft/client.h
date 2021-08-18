#pragma once

#include "common/client.h"
#include "common/pbmessage.h"
#include "common/quorumset.h"
#include "lib/assert.h"
#include "lib/signature.h"
#include "replication/minbft/minbft-proto.pb.h"

namespace dsnet {
namespace minbft {
class MinBFTClient : public Client {
 public:
  MinBFTClient(const Configuration &c, ReplicaAddress &addr, Transport *t,
               const Security &s);
  virtual ~MinBFTClient();
  virtual void ReceiveMessage(const TransportAddress &remote, void *buf,
                              size_t len) override {
    proto::MinBFTMessage msg;
    PBMessage pb_msg(msg);
    pb_msg.Parse(buf, len);
    switch (msg.msg_case()) {
      case proto::MinBFTMessage::kReply:
        HandleReply(remote, *msg.mutable_reply());
        break;
      default:
        Panic("unexpected message case %d", msg.msg_case());
    }
  }
  virtual void Invoke(const string &op, continuation_t then) override {
    this->op = op;
    this->then = then;
    SendRequest();
    req_timeout->Start();
  }
  virtual void InvokeUnlogged(
      int replica_id, const string &op, continuation_t then,
      timeout_continuation_t timeout_then,
      uint32_t timeout = DEFAULT_UNLOGGED_OP_TIMEOUT) override {
    NOT_IMPLEMENTED();
  }

 private:
  const Security &s;

  opnum_t op_num = 0;
  string op;
  continuation_t then;
  Timeout *req_timeout;
  ByzantineQuorumSet<opnum_t, string> reply_set;

  void SendRequest() {
    proto::MinBFTMessage msg;
    auto &req = *msg.mutable_request();
    req.mutable_common()->set_op(op);
    req.mutable_common()->set_clientid(clientid);
    req.mutable_common()->set_clientreqid(op_num);
    req.mutable_common()->set_clientaddr(node_addr_->Serialize());
    s.ClientSigner().Sign(req.SerializeAsString(), *req.mutable_sig());
    transport->SendMessageToAll(this, PBMessage(msg));
  }
  void HandleReply(const TransportAddress &remote, proto::ReplyMessage &msg) {
    if (!then) {
      Warning("No pending request");
      return;
    }
    string sig = msg.sig();
    msg.set_sig(string());
    if (!s.ReplicaVerifier(msg.replicaid())
             .Verify(msg.SerializeAsString(), sig)) {
      Warning("Incorrect replica #%d signature", msg.replicaid());
      return;
    }
    if (msg.clientreqid() != op_num) {
      return;
    }
    if (!reply_set.Add(op_num, msg.replicaid(), msg.reply())) {
      return;
    }
    Debug("Op #%lu finished", op_num);
    continuation_t then = this->then;
    this->then = nullptr;
    then(op, msg.reply());
  }
};
}  // namespace minbft
}  // namespace dsnet