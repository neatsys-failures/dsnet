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
  bool operator()(const string &msg, string &sig) {
    last_ui += 1;
    string raw =
        msg + std::to_string(last_ui);  // C++ string cannot have embedded 0
    return signer.Sign(raw, sig);
  }
  uint64_t UI() const { return last_ui; }
};

class VerifyUI {
  const Verifier &verifier;
  ui_t last_ui;

 public:
  VerifyUI(const Verifier &verifier) : verifier(verifier), last_ui(0) {}
  bool operator()(const string &msg, const string &sig, uint64_t ui) {
    if (ui != last_ui + 1) {
      return false;
    }
    string raw = msg + std::to_string(last_ui + 1);
    if (!verifier.Verify(raw, sig)) {
      return false;
    }
    last_ui += 1;
    return true;
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
      case proto::MinBFTMessage::kPrepare:
        HandlePrepare(remote, *msg.mutable_prepare());
        break;
      case proto::MinBFTMessage::kCommit:
        HandleCommit(remote, *msg.mutable_commit());
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

  struct PendingPrepare {
    proto::PrepareMessage msg;
    Timeout *timeout;
  };
  std::list<PendingPrepare> prepare_list;

  void HandleRequest(const TransportAddress &remote,
                     proto::RequestMessage &msg);
  void HandlePrepare(const TransportAddress &remote,
                     proto::PrepareMessage &msg);
  void HandleCommit(const TransportAddress &remote, proto::CommitMessage &msg);

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