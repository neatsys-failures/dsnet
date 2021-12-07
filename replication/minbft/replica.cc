#include "replication/minbft/replica.h"
#include "common/pbmessage.h"
#include "replication/minbft/adapter.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace minbft {

using std::map;
using std::string;
using std::unique_ptr;

MinBFTReplica::MinBFTReplica(
    const Configuration &config, int replica_id, const std::string &identifier,
    int n_worker, int batch_size, Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_id, true, transport, app),
      identifier(identifier), runner(n_worker) //
{
    for (int i = 0; i < config.n; i += 1) {
        ui_queue[i] = map<opnum_t, PendingUIMessage>();
        next_ui[i] = 1;
    }
}

MinBFTReplica::~MinBFTReplica() {
    //
}

void MinBFTReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t len //
) {
    runner.RunPrologue(
        [ //
            this, escaping_remote = remote.clone(),
            owned_buffer = string((const char *)buf, len) //
    ]() -> Runner::Solo {
            auto remote = unique_ptr<TransportAddress>(escaping_remote);
            proto::MinBFTMessage m;
            PBMessage pb_m(m);
            pb_m.Parse(owned_buffer.data(), owned_buffer.size());
            switch (m.sub_case()) {
            case proto::MinBFTMessage::SubCase::kSignedRequest: {
                Request request;
                PBMessage pb_request(request);
                SignedAdapter signed_request(pb_request, "");
                signed_request.Parse(
                    m.signed_request().data(), m.signed_request().size());
                if (!signed_request.IsVerified()) {
                    RWarning("Failed to verify request");
                    return nullptr;
                }
                // handle
            }
            case proto::MinBFTMessage::SubCase::kUiMessage: {
                proto::UIMessage ui_message;
                PBMessage pb_layer(ui_message);
                MinBFTAdapter minbft_layer(&pb_layer, "", false);
                minbft_layer.Parse(
                    m.ui_message().data(), m.ui_message().size());
                if (!minbft_layer.IsVerified()) {
                    RWarning("Failed to verify UI message");
                    return nullptr;
                }

                // do we need to verify anything further?
                // return [ //
                //            this, escaping_remote = remote.release(), ui_message,
                //            ui = minbft_layer.GetUI(), request] {
                //     auto remote = unique_ptr<TransportAddress>(escaping_remote);
                //     HandleUIMessage(*remote, ui_message, ui, request);
                // };
            }
            default:
                RPanic("Unexpected message case: %d", m.sub_case());
            }

            return nullptr;
        });
}

void MinBFTReplica::HandleUIMessage(
    const TransportAddress &remote, const proto::UIMessage &ui_message,
    opnum_t ui, const Request &request //
) {
    int remote_id = ui_message.replica_id();
    if (ui != next_ui[remote_id]) {
        PendingUIMessage pending;
        pending.ui_message = ui_message;
        pending.request = request;
        ui_queue[remote_id][ui] = pending;
        return;
    }

    next_ui[remote_id] += 1;
    HandleUIMessageInternal(remote, ui_message, ui, request);
    auto iter = ui_queue[remote_id].begin();
    while (iter != ui_queue[remote_id].end()) {
        if (iter->first != next_ui[remote_id]) {
            break;
        }
        next_ui[remote_id] += 1;
        HandleUIMessageInternal(
            remote, iter->second.ui_message, iter->first, iter->second.request);
        iter = ui_queue[remote_id].erase(iter);
    }
}

void MinBFTReplica::HandleUIMessageInternal(
    const TransportAddress &remote, const proto::UIMessage &ui_message,
    opnum_t ui, const Request &request //
) {
    switch (ui_message.sub_case()) {
    case proto::UIMessage::SubCase::kPrepare:
        HandlePrepare(remote, ui_message.prepare(), ui);
        break;
    case proto::UIMessage::SubCase::kCommit:
        HandleCommit(remote, ui_message.commit(), ui, request);
        break;
    default:
        NOT_REACHABLE();
    }
}

void MinBFTReplica::HandlePrepare(
    const TransportAddress &remote, const proto::Prepare &prepare, opnum_t ui //
) {
    if (prepare.view_number() < view_number) {
        return;
    }
    if (prepare.view_number() > view_number) {
        NOT_IMPLEMENTED(); // state transfer
    }
    if (configuration.GetLeaderIndex(view_number) == replicaIdx) {
        NOT_REACHABLE();
    }

    proto::UIMessage ui_message;
    auto &commit = *ui_message.mutable_commit();
    commit.set_view_number(view_number);
    commit.set_replica_id(replicaIdx);
    // commit.set_primary_id(prepare.replica_id());
    // commit.set_signed_request(prepare.signed_request());
    // commit.set_primary_ui(ui);
    // // acquire UI sequetially
    // MinBFTAdapter minbft_layer(nullptr, identifier, true);

    // runner.RunEpilogue([this, ui_message, minbft_layer]() mutable {
    //     PBMessage pb_layer(ui_message);
    //     minbft_layer.SetInner(&pb_layer);
    //     string ui_buffer;
    //     ui_buffer.resize(minbft_layer.SerializedSize());
    //     minbft_layer.Serialize(&ui_buffer.front());
    //     proto::MinBFTMessage m;
    //     *m.mutable_ui_message() = ui_buffer;
    //     transport->SendMessageToAll(this, PBMessage(m));
    // });
}

void MinBFTReplica::HandleCommit(
    const TransportAddress &remote, const proto::Commit &commit, opnum_t ui,
    const Request &request //
) {
    //
}

} // namespace minbft
} // namespace dsnet