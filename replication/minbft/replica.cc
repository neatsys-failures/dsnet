#include "replication/minbft/replica.h"

namespace dsnet {
namespace minbft {
void MinBFTReplica::HandleRequest(const TransportAddress &remote,
                                  proto::RequestMessage &msg) {
  if (configuration.GetLeaderIndex(view) != replicaIdx) {
    return;
  }
  string sig = msg.sig();
  msg.clear_sig();
  if (!s.ClientVerifier().Verify(msg.SerializeAsString(), sig)) {
    RWarning("Incorrect client signature");
    return;
  }
  if (reply_table.count(msg.common().clientid())) {
    RDebug("Duplicated client request #%lu", msg.common().clientreqid());
    auto &reply = reply_table[msg.common().clientid()];
    if (reply.clientreqid() == msg.common().clientreqid()) {
      proto::MinBFTMessage msg;
      *msg.mutable_reply() = reply;
      transport->SendMessage(this, remote, PBMessage(msg));
    }
    return;
  }

  RDebug("Assign UI #%lu to request, clientid = %lu, reqid = %lu",
         create_ui.UI() + 1, msg.common().clientid(),
         msg.common().clientreqid());
  proto::MinBFTMessage prepare_msg;
  auto &prepare = *prepare_msg.mutable_prepare();
  prepare.set_view(view);
  prepare.set_replicaid(replicaIdx);
  *prepare.mutable_req() = msg;
  prepare.clear_ui();
  create_ui(prepare.SerializeAsString(), *prepare.mutable_ui()->mutable_sig());
  prepare.mutable_ui()->set_id(create_ui.UI());

  log.Append(new MinBFTLogEntry(view, prepare.ui(), LOG_STATE_RECEIVED,
                                prepare.req().common(), prepare.req().sig()));

  transport->SendMessageToAll(this, PBMessage(prepare_msg));
  // TODO pending resend

  TryEnterPrepared();
}

void MinBFTReplica::HandlePrepare(const TransportAddress &remote,
                                  proto::PrepareMessage &msg) {
  if (configuration.GetLeaderIndex(view) == replicaIdx) {
    RWarning("Unexpected Prepare received");
    return;
  }
  if (msg.view() != view ||
      (int)msg.replicaid() != configuration.GetLeaderIndex(view)) {
    return;
  }
  if (auto entry = log.Find(msg.ui().id())) {
    if (entry->state != LOG_STATE_EMPTY) {
      return;
    }
  }
  proto::UI ui = msg.ui();
  msg.clear_ui();
  if (!(*verify_ui[msg.replicaid()])(msg.SerializeAsString(), ui.sig(),
                                     ui.id())) {
    RWarning("Incorrect UI for Prepare");
    return;
  }

  RDebug("Received Prepare #%lu", ui.id());
  for (uint64_t seqnum = log.LastOpnum(); seqnum + 1 < ui.id(); seqnum += 1) {
    proto::UI ui;
    log.Append(
        new MinBFTLogEntry(view, ui, LOG_STATE_EMPTY, Request(), string()));
  }
  log.Append(new MinBFTLogEntry(view, ui, LOG_STATE_RECEIVED,
                                msg.req().common(), msg.req().sig()));

  TryEnterPrepared();
}

void MinBFTReplica::HandleCommit(const TransportAddress &remote,
                                 proto::CommitMessage &msg) {
  if (msg.view() != view) {
    return;
  }
  if (msg.ui().id() <= last_executed) {
    return;
  }
  proto::UI ui = msg.ui();
  msg.clear_ui();
  if (!(*verify_ui[msg.replicaid()])(msg.SerializeAsString(), ui.sig(),
                                     ui.id())) {
    RWarning("Incorrect UI for Commit");
    return;
  }
  // TODO missing Prepare case

  if (!commit_set.AddAndCheckForQuorum(msg.ui().id(), msg.replicaid(), msg)) {
    return;
  }

  RDebug("Committed for op %lu", msg.ui().id());
  log.Find(msg.ui().id())->state = LOG_STATE_COMMITTED;
  TryExecute();
}

void MinBFTReplica::TryEnterPrepared() {
  auto entry = log.Find(last_executed + 1);
  if (!entry || entry->state == LOG_STATE_EMPTY) {
    return;
  }

  RDebug("Prepared for op #%lu", last_executed + 1);
  entry->state = LOG_STATE_PREPARED;
  proto::MinBFTMessage msg;
  auto &commit = *msg.mutable_commit();
  commit.set_view(view);
  commit.set_replicaid(replicaIdx);
  // commit.set_primaryid() TODO
  *commit.mutable_req()->mutable_common() = entry->request;
  // TODO primary ui
  commit.clear_ui();
  create_ui(commit.SerializeAsString(), *commit.mutable_ui()->mutable_sig());
  commit.mutable_ui()->set_id(create_ui.UI());

  commit_set.Add(commit.ui().id(), replicaIdx, commit);
  transport->SendMessageToAll(this, PBMessage(msg));
}

void MinBFTReplica::TryExecute() {
  while (auto entry = log.Find(last_executed + 1)) {
    if (entry->state != LOG_STATE_COMMITTED) {
      break;
    }
    RDebug("Execute for op %lu", last_executed + 1);
    entry->state = LOG_STATE_EXECUTED;
    last_executed += 1;

    const Request &req = entry->request;
    if (reply_table.count(req.clientid()) &&
        req.clientreqid() <= reply_table[req.clientid()].clientreqid()) {
      RNotice("Skip duplicated Entry");
      continue;
    }

    proto::MinBFTMessage msg;
    auto &reply = *msg.mutable_reply();
    Execute(last_executed, req, reply);
    reply_table[req.clientid()] = reply;
    transport->SendMessage(
        this, *transport->LookupAddress(ReplicaAddress(req.clientaddr())),
        PBMessage(msg));
  }
  TryEnterPrepared();
}

}  // namespace minbft
}  // namespace dsnet