#include "replication/signedunrep/replica.h"

#include "lib/message.h"
#include "lib/assert.h"
#include "lib/transport.h"
#include "lib/latency.h"
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

DEFINE_LATENCY(replica_total);
DEFINE_LATENCY(handle_request);

void SignedUnrepReplica::HandleRequest(
    const TransportAddress &remote,
    const proto::RequestMessage &msg) 
{
    Latency_Start(&handle_request);
    auto kv = clientTable.find(msg.req().clientid());
    if (kv != clientTable.end()) {
        ClientTableEntry &entry = kv->second;
        if (msg.req().clientreqid() < entry.lastReqId) {
            Latency_EndType(&handle_request, 'n');
            return;
        }
        if (msg.req().clientreqid() == entry.lastReqId) {
            PBMessage pb_m(entry.reply);
            if (!(transport->SendMessage(this, remote, SignedAdapter(pb_m, identifier)))) {
                Warning("Failed to resend reply to client");
            }
            Latency_EndType(&handle_request, 'r');
            return;
        }
    }

    last_op += 1;
    viewstamp_t v(0, last_op);
    log.Append(new LogEntry(v, LOG_STATE_RECEIVED, msg.req()));

    auto m = unique_ptr<ToClientMessage>(new ToClientMessage);
    ReplyMessage *reply = m->mutable_reply();
    Execute(last_op, msg.req(), *reply);
    // The protocol defines these as required, even if they're not
    // meaningful.
    reply->set_view(0);
    reply->set_opnum(last_op);
    *reply->mutable_req() = msg.req();

    UpdateClientTable(msg.req(), *m);

    runner.RunEpilogue([
        this,
        leaked_remote = remote.clone(),
        leaked_m = m.release()
    ]() {
        auto remote = unique_ptr<TransportAddress>(leaked_remote);
        auto m = unique_ptr<ToClientMessage>(leaked_m);

        // Latency_Start(&replica_epilogue);
        PBMessage pb_m(*m);
        if (!(transport->SendMessage(this, *remote, SignedAdapter(pb_m, identifier))))
            Warning("Failed to send reply message");
        // Latency_End(&replica_epilogue);
    });
    Latency_End(&handle_request);
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
    log(false), identifier(identifier), runner(nb_worker_thread)
{
#ifdef DSNET_NO_SIGN
    Notice("Signing is disabled");
#endif

    this->status = STATUS_NORMAL;
    this->last_op = 0;
}

SignedUnrepReplica::~SignedUnrepReplica() {
    Latency_Dump(&replica_total);
    Latency_Dump(&handle_request);
}

void SignedUnrepReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size)
{
    runner.RunPrologue([
        this, 
        leaked_remote = remote.clone(), 
        owned_buffer = string((const char *)buf, size)
    ]() mutable -> Runner::Solo {
        auto owned_remote = unique_ptr<TransportAddress>(leaked_remote);
        auto replica_msg = unique_ptr<ToReplicaMessage>(new ToReplicaMessage);
        PBMessage m(*replica_msg);
        (void) this->configuration;  // TODO get remote id from configure
        SignedAdapter signed_adapter(m, "Steve");
        signed_adapter.Parse(owned_buffer.data(), owned_buffer.size());
        if (!signed_adapter.IsVerified()) {
            Warning("Deny ill-formed message");
            return nullptr;
        }
        switch (replica_msg->msg_case()) {
            case ToReplicaMessage::MsgCase::kRequest:
                return [
                    this,
                    leaked_remote = owned_remote.release(),
                    leaked_replica_msg = replica_msg.release()
                ]() {
                    auto remote = unique_ptr<TransportAddress>(leaked_remote);
                    auto replica_msg = unique_ptr<ToReplicaMessage>(leaked_replica_msg);
                    HandleRequest(*remote, replica_msg->request());
                };
            case ToReplicaMessage::MsgCase::kUnloggedRequest:
                return [
                    this,
                    leaked_remote = owned_remote.release(),
                    leaked_replica_msg = replica_msg.release()
                ]() {
                    auto remote = unique_ptr<TransportAddress>(leaked_remote);
                    auto replica_msg = unique_ptr<ToReplicaMessage>(leaked_replica_msg);
                    HandleUnloggedRequest(*remote, replica_msg->unlogged_request());
                };
            default:
                Panic("Received unexpected message type: %u", replica_msg->msg_case());
                return nullptr;
        }
    });
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
