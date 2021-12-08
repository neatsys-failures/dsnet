#include "replication/signedunrep/replica.h"

#include "common/pbmessage.h"
#include "common/replica.h"
#include "common/signedadapter.h"
#include "lib/assert.h"
#include "lib/latency.h"
#include "lib/message.h"
#include "lib/transport.h"

#include <cstring>

namespace dsnet {
namespace signedunrep {

using namespace proto;
using std::memcpy;
using std::move;
using std::string;
using std::unique_ptr;
using std::vector;

DEFINE_LATENCY(replica_total);
DEFINE_LATENCY(handle_request);

void SignedUnrepReplica::HandleRequest(
    const TransportAddress &remote, const proto::RequestMessage &msg) {
    // Latency_Start(&handle_request);
    auto kv = clientTable.find(msg.req().clientid());
    if (kv != clientTable.end()) {
        ClientTableEntry &entry = kv->second;
        if (msg.req().clientreqid() < entry.lastReqId) {
            Latency_EndType(&handle_request, 'n');
            return;
        }
        if (msg.req().clientreqid() == entry.lastReqId) {
            PBMessage pb_m(entry.reply);
            if (!(transport->SendMessage(
                    this, remote, SignedAdapter(pb_m, "Alex")))) {
                Warning("Failed to resend reply to client");
            }
            Latency_EndType(&handle_request, 'r');
            return;
        }
    } else {
        ClientTableEntry entry;
        entry.lastReqId = 0; // make sure reply always get updated later
        entry.remote = unique_ptr<TransportAddress>(remote.clone());
        clientTable.emplace(msg.req().clientid(), move(entry));
    }

    last_op += 1;
    viewstamp_t v(0, last_op);
    log.Append(new LogEntry(v, LOG_STATE_RECEIVED, msg.req()));

    request_batch.push_back(msg.req());
    if (!close_batch_timeout->Active()) {
        close_batch_timeout->Start();
    }
    if (request_batch.size() >= batch_size) {
        CloseBatch();
    }

    // Latency_End(&handle_request);
}

void SignedUnrepReplica::CloseBatch() {
    close_batch_timeout->Stop();
    vector<Runner::Epilogue> epilogue_list;
    for (int i = 0; i < request_batch.size(); i += 1) {

        ToClientMessage m;
        ReplyMessage *reply = m.mutable_reply();
        Execute(last_op, request_batch[i], *reply);
        // The protocol defines these as required, even if they're not
        // meaningful.
        reply->set_view(0);
        reply->set_opnum(last_op);
        *reply->mutable_req() = request_batch[i];

        UpdateClientTable(request_batch[i], m);

        epilogue_list.push_back(
            [ //
                this,
                leaked_remote =
                    clientTable.at(request_batch[i].clientid()).remote->clone(),
                m //
        ]() mutable {
                auto remote = unique_ptr<TransportAddress>(leaked_remote);
                PBMessage pb_m(m);
                if (!(transport->SendMessage(
                        this, *remote, SignedAdapter(pb_m, identifier, false))))
                    Warning("Failed to send reply message");
            });
    }
    request_batch.clear();
    runner.RunEpilogue([epilogue_list] {
        for (Runner::Epilogue epilogue : epilogue_list) {
            epilogue();
        }
    });
}

void SignedUnrepReplica::HandleUnloggedRequest(
    const TransportAddress &remote, const UnloggedRequestMessage &msg) {
    NOT_REACHABLE();
}

SignedUnrepReplica::SignedUnrepReplica(
    Configuration config, string identifier, int n_worker, int batch_size,
    Transport *transport, AppReplica *app)
    : Replica(config, 0, 0, true, transport, app), log(false),
      identifier(identifier), runner(n_worker), batch_size(batch_size) {

    this->status = STATUS_NORMAL;
    this->last_op = 0;

    close_batch_timeout = unique_ptr<Timeout>(
        new Timeout(transport, 10, [this] { CloseBatch(); }));
}

SignedUnrepReplica::~SignedUnrepReplica() {
    Latency_Dump(&replica_total);
    Latency_Dump(&handle_request);
}

void SignedUnrepReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size) {
    runner.RunPrologue(
        [ //
            this, leaked_remote = remote.clone(),
            owned_buffer = string((const char *)buf, size) //
    ]() mutable -> Runner::Solo {
            auto owned_remote = unique_ptr<TransportAddress>(leaked_remote);
            ToReplicaMessage replica_msg;
            PBMessage m(replica_msg);
            (void)this->configuration; // TODO get remote id from configure
            SignedAdapter signed_adapter(m, "Steve");
            signed_adapter.Parse(owned_buffer.data(), owned_buffer.size());
            if (!signed_adapter.IsVerified()) {
                Warning("Deny ill-formed message");
                return nullptr;
            }
            switch (replica_msg.msg_case()) {
            case ToReplicaMessage::MsgCase::kRequest:
                return [this, leaked_remote = owned_remote.release(),
                        replica_msg]() {
                    auto remote = unique_ptr<TransportAddress>(leaked_remote);
                    HandleRequest(*remote, replica_msg.request());
                };
            default:
                Panic(
                    "Received unexpected message type: %u",
                    replica_msg.msg_case());
                return nullptr;
            }
        });
}

void SignedUnrepReplica::UpdateClientTable(
    const Request &req, const ToClientMessage &reply) {
    ClientTableEntry &entry = clientTable[req.clientid()];

    ASSERT(entry.lastReqId <= req.clientreqid());
    if (entry.lastReqId == req.clientreqid()) {
        // Duplicate request
        return;
    }

    entry.lastReqId = req.clientreqid();
    entry.reply = reply;
}

} // namespace signedunrep
} // namespace dsnet
