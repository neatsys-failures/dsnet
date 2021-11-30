#include "replication/pbft/replica.h"
#include "common/pbmessage.h"
#include "common/signedadapter.h"
#include "sequencer/sequencer.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace pbft {

using std::move;
using std::string;
using std::unique_ptr;

PBFTReplica::PBFTReplica(
    const Configuration &config, int replica_id, const string &identifier,
    int n_worker, int batch_size, Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_id, true, transport, app),
      identifier(identifier), runner(n_worker), batch_size(batch_size),
      view_number(0), op_number(0), commit_number(0), log(false) //
{
    //
}

PBFTReplica::~PBFTReplica() {
    //
}

void PBFTReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t len //
) {
    runner.RunPrologue(
        [ //
            this, escaping_remote = remote.clone(),
            owned_buffer = string((const char *)buf, len) //
    ]() -> Runner::Solo {
            auto remote = unique_ptr<TransportAddress>(escaping_remote);
            proto::PBFTMessage message;
            PBMessage pb_layer(message);
            SignedAdapter signed_layer(pb_layer, "");
            signed_layer.Parse(owned_buffer.data(), owned_buffer.size());
            if (!signed_layer.IsVerified()) {
                RWarning("Receive message failed to verify");
                return nullptr;
            }
            switch (message.sub_case()) {
            case proto::PBFTMessage::SubCase::kRequest:
                return [ //
                           this, escaping_remote = remote.release(), message,
                           owned_buffer, digest = signed_layer.Digest() //
                ]() {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    HandleRequest(
                        *remote, message.request(), owned_buffer, digest);
                };
            default:
                RPanic("Unexpected message case: %d", message.sub_case());
            }
            return nullptr;
        });
}

void PBFTReplica::HandleRequest(
    const TransportAddress &remote, const Request &request,
    const string &signed_message, const string &digest //
) {
    const auto &iter = client_table.find(request.clientid());
    if (iter != client_table.end()) {
        auto &entry = iter->second;
        if (entry.request_number > request.clientreqid()) {
            return;
        }
        if (entry.request_number == request.clientreqid()) {
            if (entry.has_reply) {
                PBMessage pb_reply(entry.reply);
                transport->SendMessage(this, *entry.remote, pb_reply);
            }
            return;
        }
    } else {
        ClientEntry entry;
        entry.remote = unique_ptr<TransportAddress>(
            transport->LookupAddress(ReplicaAddress(request.clientaddr())));
        entry.request_number = request.clientreqid();
        entry.has_reply = false;
        client_table.emplace(request.clientid(), move(entry));
    }

    if (!IsPrimary()) {
        transport->SendMessageToReplica(
            this, configuration.GetLeaderIndex(view_number),
            BufferMessage(signed_message.data(), signed_message.size()));
        // TODO schedule view change
        return;
    }

    op_number += 1;
    log.Append(new LogEntry(
        viewstamp_t(view_number, op_number), LOG_STATE_RECEIVED, request));

    proto::PBFTMessage message;
    auto &preprepare = *message.mutable_preprepare();
    preprepare.set_view_number(view_number);
    preprepare.set_op_number(op_number);
    preprepare.set_digest(digest);
    preprepare.set_signed_message(signed_message);
    runner.RunEpilogue([this, message]() mutable {
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier);
        transport->SendMessageToAll(this, signed_layer);
    });
}

void PBFTReplica::HandlePreprepare(
    const TransportAddress &remote, const proto::Preprepare &preprepare,
    const Request &request //
) {
    //
}

} // namespace pbft
} // namespace dsnet