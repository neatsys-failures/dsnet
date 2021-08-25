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
  if (reply_table.count(msg.common().clientid()) &&
      reply_table[msg.common().clientid()].clientreqid() >=
          msg.common().clientreqid()) {
    RDebug("Duplicated client request #%lu", msg.common().clientreqid());
    auto &reply = reply_table[msg.common().clientid()];
    if (reply.clientreqid() == msg.common().clientreqid()) {
      proto::MinBFTMessage msg;
      *msg.mutable_reply() = reply;
      transport->SendMessage(this, remote, PBMessage(msg));
    }
    return;
  }

  RDebug("Prepare clientid = %lu, reqid = %lu", msg.common().clientid(),
         msg.common().clientreqid());
  proto::MinBFTMessage prepare_msg;
  auto &prepare = *prepare_msg.mutable_prepare();
  prepare.set_view(view);
  prepare.set_replicaid(replicaIdx);
  *prepare.mutable_req() = msg;
  prepare.clear_ui();
  {
    string sig;
    create_ui(prepare.SerializeAsString(), sig);
    prepare.mutable_ui()->set_sig(sig);
  }
  prepare.mutable_ui()->set_id(create_ui.UI());

  transport->SendMessageToAll(this, PBMessage(prepare_msg));
  // TODO pending resend

  seqnum += 1;
  log.Append(new MinBFTLogEntry(view, seqnum, LOG_STATE_RECEIVED, msg.common(),
                                msg.sig(), prepare.ui()));
  TryEnterPrepared();
}

void MinBFTReplica::HandlePrepare(const TransportAddress &remote,
                                  proto::PrepareMessage &msg) {
  proto::UI ui = msg.ui();
  msg.clear_ui();
  if (!(*verify_ui[msg.replicaid()])(msg.SerializeAsString(), ui.sig(),
                                     ui.id())) {
    RWarning("Incorrect UI for Prepare");
    return;
  }
  if (configuration.GetLeaderIndex(view) == replicaIdx) {
    RWarning("Unexpected Prepare received");
    return;
  }
  if (msg.view() != view ||
      (int)msg.replicaid() != configuration.GetLeaderIndex(view)) {
    return;
  }
  // if (auto entry = log.Find(msg.ui().id())) {
  //   if (entry->state != LOG_STATE_EMPTY) {
  //     return;
  //   }
  // }

  seqnum += 1;
  RDebug("Received Prepare #%lu (assumed)", seqnum);
  log.Append(new MinBFTLogEntry(view, seqnum, LOG_STATE_RECEIVED,
                                msg.req().common(), msg.req().sig(), ui));

  TryEnterPrepared();
}

void MinBFTReplica::HandleCommit(const TransportAddress &remote,
                                 proto::CommitMessage &msg) {
  proto::UI ui = msg.ui();
  msg.clear_ui();
  if (!(*verify_ui[msg.replicaid()])(msg.SerializeAsString(), ui.sig(),
                                     ui.id())) {
    RWarning("Incorrect UI for Commit, from replica %d", msg.replicaid());
    return;
  }
  if (msg.view() != view) {
    return;
  }
  auto entry = FindEntry(msg.primaryui().id());
  if (entry->viewstamp.opnum <= last_executed) {
    return;
  }
  // TODO verify primary UI
  // TODO missing Prepare case

  // RDebug("on Commit #%lu", entry->viewstamp.opnum);
  if (!commit_set.AddAndCheckForQuorum(entry->viewstamp.opnum, msg.replicaid(),
                                       msg)) {
    return;
  }

  RDebug("Committed for op %lu", ui.id());
  entry->state = LOG_STATE_COMMITTED;
  TryExecute();
}

void MinBFTReplica::TryEnterPrepared() {
  auto entry = log.Find(last_executed + 1);
  if (!entry || entry->state != LOG_STATE_RECEIVED) {
    return;
  }

  RDebug("Prepared for op #%lu", last_executed + 1);
  entry->state = LOG_STATE_PREPARED;
  proto::MinBFTMessage msg;
  auto &commit = *msg.mutable_commit();
  commit.set_view(entry->viewstamp.view);
  commit.set_replicaid(replicaIdx);
  commit.set_primaryid(configuration.GetLeaderIndex(entry->viewstamp.view));
  *commit.mutable_req()->mutable_common() = entry->request;
  *commit.mutable_primaryui() = entry->As<MinBFTLogEntry>().ui;
  commit.clear_ui();
  string sig;
  create_ui(commit.SerializeAsString(), sig);
  commit.mutable_ui()->set_sig(sig);
  commit.mutable_ui()->set_id(create_ui.UI());

  commit_set.Add(entry->viewstamp.opnum, replicaIdx, commit);
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
    reply.set_replicaid(replicaIdx);
    reply.set_clientreqid(req.clientreqid());
    Execute(last_executed, req, reply);
    reply.clear_sig();
    s.ReplicaSigner(replicaIdx)
        .Sign(reply.SerializeAsString(), *reply.mutable_sig());
    reply_table[req.clientid()] = reply;
    transport->SendMessage(
        this, *transport->LookupAddress(ReplicaAddress(req.clientaddr())),
        PBMessage(msg));
  }
  TryEnterPrepared();
}

}  // namespace minbft
}  // namespace dsnet