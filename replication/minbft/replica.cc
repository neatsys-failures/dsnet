#include "replication/minbft/replica.h"

#include "lib/assert.h"

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
  auto &from_replica = *prepare_msg.mutable_from_replica();
  auto &prepare = *from_replica.mutable_prepare();
  prepare.set_view(view);
  *prepare.mutable_req() = msg;
  from_replica.set_replicaid(replicaIdx);
  from_replica.clear_ui();
  proto::UI ui;
  create_ui(from_replica.SerializeAsString(), ui);
  *from_replica.mutable_ui() = ui;

  transport->SendMessageToAll(this, PBMessage(prepare_msg));
  // TODO pending resend

  seqnum += 1;
  log.Append(new MinBFTLogEntry(view, seqnum, LOG_STATE_RECEIVED, msg.common(),
                                msg.sig(), prepare_msg.from_replica().ui()));
  TryEnterPrepared();
}

void MinBFTReplica::EnqueueReplicaMessage(proto::FromReplicaMessage &msg) {
  proto::UI ui = msg.ui();
  msg.clear_ui();
  if (!(*verify_ui[msg.replicaid()])(msg.SerializeAsString(), ui.sig(),
                                     ui.id())) {
    RWarning("Wrong signature for Prepare/Commit");
    return;
  }
  *msg.mutable_ui() = ui;
  msg_queue[msg.replicaid()][msg.ui().id()] = msg;
  while (msg_queue[msg.replicaid()].size()) {
    auto iter = msg_queue[msg.replicaid()].begin();
    auto next_msg = iter->second;
    msg_queue[msg.replicaid()].erase(iter);
    if (next_msg.ui().id() != last_ui[msg.replicaid()] + 1) {
      break;
    }
    last_ui[msg.replicaid()] += 1;
    switch (next_msg.msg_case()) {
      case proto::FromReplicaMessage::kPrepare:
        HandlePrepare(next_msg);
        break;
      case proto::FromReplicaMessage::kCommit:
        HandleCommit(next_msg);
        break;
      default:
        NOT_REACHABLE();
    }
  }
}

void MinBFTReplica::HandlePrepare(proto::FromReplicaMessage &big_msg) {
  proto::PrepareMessage &msg = *big_msg.mutable_prepare();
  if (configuration.GetLeaderIndex(view) == replicaIdx) {
    RWarning("Unexpected Prepare received");
    return;
  }
  if (msg.view() != view ||
      (int)big_msg.replicaid() != configuration.GetLeaderIndex(view)) {
    return;
  }

  seqnum += 1;
  RDebug("Received Prepare #%lu (assumed)", seqnum);
  log.Append(new MinBFTLogEntry(view, seqnum, LOG_STATE_RECEIVED,
                                msg.req().common(), msg.req().sig(),
                                big_msg.ui()));

  TryEnterPrepared();
}

void MinBFTReplica::HandleCommit(proto::FromReplicaMessage &big_msg) {
  auto &msg = *big_msg.mutable_commit();
  proto::FromReplicaMessage recovered;
  recovered.mutable_prepare()->set_view(msg.view());
  recovered.set_replicaid(msg.primaryid());
  *recovered.mutable_prepare()->mutable_req() = msg.req();
  recovered.clear_ui();
  if (!(*verify_ui[msg.primaryid()])(recovered.SerializeAsString(),
                                     msg.primaryui().sig(),
                                     msg.primaryui().id())) {
    RWarning("Incorrect primary UI for Commit");
    return;
  }
  if (msg.view() != view) {
    return;
  }
  auto entry = FindEntry(msg.primaryui().id());
  if (!entry) {
    // TODO append log
    msg_queue[big_msg.replicaid()][big_msg.ui().id()] = big_msg;
    return;
  }

  if (entry->viewstamp.opnum <= last_executed) {
    return;
  }
  // TODO verify primary UI
  // TODO missing Prepare case

  // RDebug("on Commit #%lu", entry->viewstamp.opnum);
  if (!commit_set.AddAndCheckForQuorum(entry->viewstamp.opnum,
                                       big_msg.replicaid(), msg)) {
    return;
  }

  RDebug("Committed for op %lu", big_msg.ui().id());
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
  auto &from_replica = *msg.mutable_from_replica();
  from_replica.set_replicaid(replicaIdx);
  auto &commit = *from_replica.mutable_commit();
  commit.set_view(entry->viewstamp.view);
  commit.set_primaryid(configuration.GetLeaderIndex(entry->viewstamp.view));
  *commit.mutable_req()->mutable_common() = entry->request;
  *commit.mutable_primaryui() = entry->As<MinBFTLogEntry>().ui;
  from_replica.clear_ui();
  proto::UI ui;
  create_ui(from_replica.SerializeAsString(), ui);
  *from_replica.mutable_ui() = ui;

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