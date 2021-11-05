#include "replication/signedunrep/replica.h"

#include "lib/message.h"
#include "lib/assert.h"
#include "lib/transport.h"
#include "common/replica.h"
#include "common/pbmessage.h"
#include "common/signedadapter.h"

#include <cstring>

namespace dsnet {
namespace signedunrep {

using namespace proto;
using std::string;
using std::memcpy;
using std::unique_ptr;
using std::move;

void SignedUnrepReplica::HandleRequest(
    const TransportAddress &remote,
    const proto::RequestMessage &msg) 
{

    auto kv = clientTable.find(msg.req().clientid());
    if (kv != clientTable.end()) {
        ClientTableEntry &entry = kv->second;
        if (msg.req().clientreqid() < entry.lastReqId) {
            return;
        }
        if (msg.req().clientreqid() == entry.lastReqId) {
            PBMessage pb_m(entry.reply);
            if (!(transport->SendMessage(this, remote, SignedAdapter(pb_m, identifier)))) {
                Warning("Failed to resend reply to client");
            }
            return;
        }
    }

    last_op += 1;
    viewstamp_t v(0, last_op);
    log.Append(new LogEntry(v, LOG_STATE_RECEIVED, msg.req()));

    ToClientMessage m;
    ReplyMessage *reply = m.mutable_reply();
    Execute(last_op, msg.req(), *reply);
    // The protocol defines these as required, even if they're not
    // meaningful.
    reply->set_view(0);
    reply->set_opnum(last_op);
    *reply->mutable_req() = msg.req();
    PBMessage pb_m(m);
    if (!(transport->SendMessage(this, remote, SignedAdapter(pb_m, identifier))))
        Warning("Failed to send reply message");

    UpdateClientTable(msg.req(), m);
}

void SignedUnrepReplica::HandleUnloggedRequest(
    const TransportAddress &remote, const UnloggedRequestMessage &msg)
{
    NOT_REACHABLE();
}

SignedUnrepReplica::SignedUnrepReplica(
    Configuration config, string identifier, int nb_worker_thread,
    Transport *transport, AppReplica *app)
    : Replica(config, 0, 0, true, transport, app),
    log(false), identifier(identifier), prologue(nb_worker_thread)
{
    this->status = STATUS_NORMAL;
    this->last_op = 0;

    poll_timeout = new Timeout(transport, 100, [this]() {
        PollVerifiedMessage();
    });
    poll_timeout->Start();
}

SignedUnrepReplica::~SignedUnrepReplica() {
    delete poll_timeout;
}

void SignedUnrepReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size)
{
    prologue.Enqueue(
        unique_ptr<PrologueTask>(PrologueTask::New<ToReplicaMessage>(buf, size, remote)),
        [this](PrologueTask &task) {
            auto message = unique_ptr<ToReplicaMessage>(new ToReplicaMessage);
            PBMessage m(*message);
            (void) this->configuration;  // TODO get remote id from configure
            SignedAdapter signed_adapter(m, "Steve");
            task.ParseWithAdapter(signed_adapter);
            if (!signed_adapter.IsVerified()) {
                Warning("Deny ill-formed message");
                return;
            }
            task.SetMessage(move(message));
        }
    );
    PollVerifiedMessage();
}

void SignedUnrepReplica::PollVerifiedMessage() {
    while (unique_ptr<PrologueTask> verified = prologue.Dequeue()) {
        const TransportAddress &remote = verified->Remote();
        ToReplicaMessage replica_msg = verified->Message<ToReplicaMessage>();
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
