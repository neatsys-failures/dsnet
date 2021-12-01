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
            case proto::PBFTMessage::SubCase::kPreprepare: {
                const string &prepare_buffer =
                    message.preprepare().signed_prepare();
                proto::Prepare prepare_message;
                PBMessage pb_prepare(prepare_message);
                SignedAdapter signed_prepare(pb_prepare, "");
                signed_prepare.Parse(
                    prepare_buffer.data(), prepare_buffer.size());
                if (!signed_prepare.IsVerified()) {
                    RWarning("Failed to verify Preprepare (Prepare)");
                    return nullptr;
                }

                const string &request_buffer =
                    message.preprepare().signed_message();
                proto::PBFTMessage request_message;
                PBMessage pb_request(request_message);
                SignedAdapter signed_request(pb_request, "");
                signed_request.Parse(
                    request_buffer.data(), request_buffer.size());
                if (!signed_request.IsVerified() ||
                    !request_message.has_request()) {
                    RWarning("Failed to verify Preprepare (Request)");
                    return nullptr;
                }
                return [ //
                           this, escaping_remote = remote.release(), message,
                           prepare_message,
                           request_message //
                ]() {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    HandlePreprepare(
                        *remote, prepare_message, request_message.request());
                };
            }
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

    proto::Prepare prepare;
    prepare.set_view_number(view_number);
    prepare.set_op_number(op_number);
    prepare.set_digest(digest);
    prepare.set_replica_id(replicaIdx);
    runner.RunEpilogue([this, prepare, signed_message]() mutable {
        PBMessage pb_prepare(prepare);
        SignedAdapter signed_prepare(pb_prepare, identifier);
        string signed_prepare_buffer;
        signed_prepare_buffer.resize(signed_prepare.SerializedSize());
        signed_prepare.Serialize(&signed_prepare_buffer.front());

        proto::PBFTMessage message;
        auto &preprepare = *message.mutable_preprepare();
        *preprepare.mutable_signed_prepare() = signed_prepare_buffer;
        *preprepare.mutable_signed_message() = signed_message;
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier, false);
        transport->SendMessageToAll(this, signed_layer);
    });
}

void PBFTReplica::HandlePreprepare(
    const TransportAddress &remote, const proto::Prepare &prepare,
    const Request &request //
) {
    if (prepare.view_number() < view_number) {
        return;
    }
    if (prepare.view_number() > view_number) {
        NOT_IMPLEMENTED(); // state transfer
    }
    if (IsPrimary()) {
        NOT_REACHABLE();
    }
    // TODO resend for slow leader
    if (prepare.op_number() <= log.LastOpnum()) {
        return;
    }
    if (prepare.op_number() != log.LastOpnum() + 1) {
        NOT_IMPLEMENTED(); // state transfer
    }

    log.Append(new LogEntry(
        viewstamp_t(view_number, prepare.op_number()), LOG_STATE_RECEIVED,
        request));

    proto::PBFTMessage message;
    *message.mutable_prepare() = prepare;
    message.mutable_prepare()->set_replica_id(replicaIdx);
    runner.RunEpilogue([this, message]() mutable {
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier);
        transport->SendMessageToAll(this, signed_layer);
    });
}

} // namespace pbft
} // namespace dsnet