#pragma once
#include <endian.h>

#include <unordered_map>

#include "common/pbmessage.h"
#include "common/quorumset.h"
#include "common/replica.h"
#include "lib/message.h"
#include "lib/signature.h"
#include "replication/minbft/minbft-proto.pb.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace minbft {

// unique identifier acts like opnum_t in many protocols
// additional to indentify each op, it also participant in verification
// could use opnum_t directly, but define it to make things clearer
typedef uint64_t ui_t;

// To prevent confusing: Signer & Verifier should be stateless
// A MinBFT Signer/Verifier is stateful, so they are not named (and derived
// from) Signer/Verifier.
class CreateUI {
  const Signer &signer;
  ui_t last_ui;

 public:
  CreateUI(const Signer &signer) : signer(signer), last_ui(0) {}
  bool operator()(const string &msg, proto::UI &ui) {
    last_ui += 1;
    string raw =
        msg + std::to_string(last_ui);  // C++ string cannot have embedded 0
    string sig;
    if (!signer.Sign(raw, sig)) {
      return false;
    }
    ui.set_sig(sig);
    ui.set_id(last_ui);
    return true;
  }
};

class VerifyUI {
  const Verifier &verifier;

 public:
  VerifyUI(const Verifier &verifier) : verifier(verifier) {}
  bool operator()(const string &msg, const string &sig, uint64_t ui) {
    string raw = msg + std::to_string(ui);
    return verifier.Verify(raw, sig);
  }
};

class MinBFTLogEntry : public LogEntry {
 public:
  string sig;
  proto::UI ui;

  MinBFTLogEntry(view_t view, opnum_t seqnum, LogEntryState state,
                 const Request &req, string sig, proto::UI ui)
      : LogEntry(viewstamp_t(view, seqnum), state, req), sig(sig), ui(ui) {}
  virtual ~MinBFTLogEntry() {}
};

class MinBFTReplica : public Replica {
 public:
  MinBFTReplica(const Configuration &c, int replica_id, bool init, Transport *t,
                const Security &s, AppReplica *app)
      : Replica(c, 0, replica_id, init, t, app),
        s(s),
        create_ui(s.ReplicaSigner(replica_id)),
        view(0),
        seqnum(0),
        log(false),
        last_executed(0),
        commit_set(c.f + 1) {
    verify_ui = new std::unique_ptr<VerifyUI>[c.n];
    for (int i = 0; i < c.n; i += 1) {
      verify_ui[i] =
          std::unique_ptr<VerifyUI>(new VerifyUI(s.ReplicaVerifier(i)));
      last_ui[i] = 0;
    }
  }
  virtual ~MinBFTReplica() {
    delete[] verify_ui;
    //
  }
  void ReceiveMessage(const TransportAddress &remote, void *buf,
                      size_t size) override {
    proto::MinBFTMessage msg;
    PBMessage pb_msg(msg);
    pb_msg.Parse(buf, size);
    switch (msg.msg_case()) {
      case proto::MinBFTMessage::kRequest:
        HandleRequest(remote, *msg.mutable_request());
        break;
      case proto::MinBFTMessage::kFromReplica:
        EnqueueReplicaMessage(*msg.mutable_from_replica());
        break;
      case proto::MinBFTMessage::kGapRequest:
        HandleGapRequest(remote, *msg.mutable_gap_request());
        break;
      default:
        RPanic("Unexpected message case: %d", msg.msg_case());
    }
  }

 private:
  const Security &s;
  CreateUI create_ui;
  std::unique_ptr<VerifyUI> *verify_ui;

  view_t view;
  opnum_t seqnum;
  Log log;
  ui_t last_executed;
  QuorumSet<ui_t, proto::CommitMessage> commit_set;

  std::unordered_map<uint64_t, proto::ReplyMessage> reply_table;

  void HandleRequest(const TransportAddress &remote,
                     proto::RequestMessage &msg);

  void EnqueueReplicaMessage(proto::FromReplicaMessage &msg);
  std::unordered_map<int, std::map<ui_t, proto::FromReplicaMessage>> msg_queue;
  std::unordered_map<int, std::unordered_map<ui_t, std::unique_ptr<Timeout>>>
      timer_map;
  std::unordered_map<int, ui_t> last_ui;

  void Broadcast(proto::FromReplicaMessage &msg) {
    Assert(msg.ui().id() == msg_log.empty() ? 1 : msg_log.back().ui().id() + 1);
    msg_log.push_back(msg);
    proto::MinBFTMessage big_msg;
    *big_msg.mutable_from_replica() = msg;
    transport->SendMessageToAll(this, PBMessage(big_msg));
  }
  const proto::FromReplicaMessage &FindPastMessage(ui_t ui) {
    return msg_log[ui - 1];
  }
  std::vector<proto::FromReplicaMessage> msg_log;

  void HandlePrepare(proto::FromReplicaMessage &msg);
  void HandleCommit(proto::FromReplicaMessage &msg);
  void HandleGapRequest(const TransportAddress &remote,
                        proto::GapRequest &msg) {
    proto::MinBFTMessage reply_msg;
    *reply_msg.mutable_from_replica() = FindPastMessage(msg.seq());
    transport->SendMessage(this, remote, PBMessage(reply_msg));
  }

  void TryEnterPrepared();
  void TryExecute();

  MinBFTLogEntry *FindEntry(ui_t id) {
    for (opnum_t seqnum = log.LastOpnum(); seqnum >= log.FirstOpnum();
         seqnum -= 1) {
      auto &entry = log.Find(seqnum)->As<MinBFTLogEntry>();
      if (entry.ui.id() == id) {
        return &entry;
      }
    }
    return nullptr;
  }
};
}  // namespace minbft
}  // namespace dsnet