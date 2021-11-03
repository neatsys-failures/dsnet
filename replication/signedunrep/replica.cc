#include "common/replica.h"
#include "common/pbmessage.h"
#include "replication/signedunrep/replica.h"

#include "lib/message.h"
#include "lib/assert.h"
#include "lib/transport.h"

namespace dsnet {
namespace signedunrep {

using namespace proto;
using std::string;

void SignedUnrepReplica::HandleRequest(
    const TransportAddress &remote,
    const proto::RequestMessage &msg) 
{
    ToClientMessage m;
    ReplyMessage *reply = m.mutable_reply();

    auto kv = clientTable.find(msg.req().clientid());
    if (kv != clientTable.end()) {
        ClientTableEntry &entry = kv->second;
        if (msg.req().clientreqid() < entry.lastReqId) {
            return;
        }
        if (msg.req().clientreqid() == entry.lastReqId) {
            if (!(transport->SendMessage(this, remote, PBMessage(entry.reply)))) {
                Warning("Failed to resend reply to client");
            }
            return;
        }
    }

    last_op += 1;
    viewstamp_t v(0, last_op);
    log.Append(new LogEntry(v, LOG_STATE_RECEIVED, msg.req()));

    Execute(last_op, msg.req(), *reply);

    // The protocol defines these as required, even if they're not
    // meaningful.
    reply->set_view(0);
    reply->set_opnum(last_op);
    *reply->mutable_req() = msg.req();

    if (!(transport->SendMessage(this, remote, PBMessage(m))))
        Warning("Failed to send reply message");

    UpdateClientTable(msg.req(), m);
}

void SignedUnrepReplica::HandleUnloggedRequest(
    const TransportAddress &remote, const UnloggedRequestMessage &msg)
{
    ToClientMessage m;
    UnloggedReplyMessage *reply = m.mutable_unlogged_reply();

    ExecuteUnlogged(msg.req(), *reply);

    if (!(transport->SendMessage(this, remote, PBMessage(m))))
        Warning("Failed to send reply message");
}

SignedUnrepReplica::SignedUnrepReplica(
    Configuration config, string identifier, Transport *transport, AppReplica *app): 
    Replica(config, 0, 0, true, transport, app),
    log(false)
{
    this->status = STATUS_NORMAL;
    this->last_op = 0;
}

void SignedUnrepReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size)
{
    static ToReplicaMessage replica_msg;
    static PBMessage m(replica_msg);

    m.Parse(buf, size);

    switch (replica_msg.msg_case()) {
        case ToReplicaMessage::MsgCase::kRequest:
            HandleRequest(remote, replica_msg.request());
            break;
        case ToReplicaMessage::MsgCase::kUnloggedRequest:
            HandleUnloggedRequest(remote, replica_msg.unlogged_request());
            break;
        default:
            Panic("Received unexpected message type: %u", replica_msg.msg_case());
    }
}

void SignedUnrepReplica::UpdateClientTable(
    const Request &req, const ToClientMessage &reply)
{
    ClientTableEntry &entry = clientTable[req.clientid()];

    ASSERT(entry.lastReqId <= req.clientreqid());
    if (entry.lastReqId == req.clientreqid()) {
        // Duplicate request
        return;
    }

    entry.lastReqId = req.clientreqid();
    entry.reply = reply;
}

} 
} 
