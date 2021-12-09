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
using std::move;
using std::string;
using std::unique_ptr;
using std::vector;

MinBFTReplica::MinBFTReplica(
    const Configuration &config, int replica_id, const std::string &identifier,
    int n_worker, int batch_size, Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_id, true, transport, app),
      identifier(identifier), runner(n_worker), commit_quorum(config.f + 1),
      commit_number(0), log(false) //
{
    for (int i = 0; i < config.n; i += 1) {
        ui_queue[i] = map<opnum_t, Runner::Solo>();
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
                return [ //
                           this, escaping_remote = remote.release(), request,
                           owned_buffer] {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    HandleRequest(*remote, request, owned_buffer);
                };
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

                Runner::Solo solo;
                switch (ui_message.sub_case()) {
                case proto::UIMessage::SubCase::kPrepare: {
                    const string &buffer =
                        ui_message.prepare().signed_request();
                    Request request;
                    PBMessage pb_request(request);
                    SignedAdapter signed_request(pb_request, "");
                    signed_request.Parse(buffer.data(), buffer.size());
                    if (!signed_request.IsVerified()) {
                        RWarning("Failed to verify UI::Prepare::Request");
                        return nullptr;
                    }
                    solo = [ //
                               this, escaping_remote = remote.release(),
                               ui_message, ui = minbft_layer.GetUI(), request] {
                        auto remote =
                            unique_ptr<TransportAddress>(escaping_remote);
                        HandlePrepare(
                            *remote, ui_message.prepare(), ui, request);
                    };
                    break;
                }
                case proto::UIMessage::SubCase::kCommit: {
                    solo = [ //
                               this, escaping_remote = remote.release(),
                               ui_message] {
                        auto remote =
                            unique_ptr<TransportAddress>(escaping_remote);
                        HandleCommit(*remote, ui_message.commit());
                    };
                    break;
                }
                default:
                    RPanic(
                        "Unexpect UIMessage case: %d", ui_message.sub_case());
                }

                // the wrapping solo
                // I am such a genius
                return
                    [ //
                        this, solo, ui = minbft_layer.GetUI(),
                        // TODO get this from MinBFT layer instead of protobuf
                        remote_id = ui_message.replica_id() //
                ] {
                        if (ui != next_ui[remote_id]) {
                            ui_queue[remote_id][ui] = solo;
                            return;
                        }

                        next_ui[remote_id] += 1;
                        solo();
                        auto iter = ui_queue[remote_id].begin();
                        while (iter != ui_queue[remote_id].end()) {
                            if (iter->first != next_ui[remote_id]) {
                                break;
                            }
                            next_ui[remote_id] += 1;
                            iter->second();
                            iter = ui_queue[remote_id].erase(iter);
                        }
                    };
            }
            default:
                RPanic("Unexpected message case: %d", m.sub_case());
            }

            return nullptr;
        });
}

void MinBFTReplica::HandlePrepare(
    const TransportAddress &remote, const proto::Prepare &prepare, opnum_t ui,
    const Request &request //
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

    while (log.LastOpnum() + 1 != ui) {
        log.Append(new LogEntry(
            viewstamp_t(view_number, log.LastOpnum() + 1), LOG_STATE_NOOP,
            Request()));
    }
    log.Append(new LogEntry(
        viewstamp_t(view_number, ui), LOG_STATE_PREPARED, request));

    SendCommit(ui);
    ConcludeEpilogue();
}

void MinBFTReplica::SendCommit(opnum_t ui) {
    proto::UIMessage ui_message;
    auto &commit = *ui_message.mutable_commit();
    commit.set_view_number(view_number);
    commit.set_replica_id(replicaIdx);
    commit.set_primary_ui(ui);
    // acquire UI sequetially
    MinBFTAdapter minbft_layer(nullptr, identifier, true);

    epilogue_list.push_back([this, ui_message, minbft_layer]() mutable {
        PBMessage pb_layer(ui_message);
        minbft_layer.SetInner(&pb_layer);
        string ui_buffer;
        ui_buffer.resize(minbft_layer.SerializedSize());
        minbft_layer.Serialize(&ui_buffer.front());
        proto::MinBFTMessage m;
        *m.mutable_ui_message() = ui_buffer;
        transport->SendMessageToAll(this, PBMessage(m));
    });
    AddCommit(commit);
    ConcludeEpilogue();
}

void MinBFTReplica::HandleCommit(
    const TransportAddress &remote, const proto::Commit &commit //
) {
    AddCommit(commit);
    ConcludeEpilogue();
}

void MinBFTReplica::AddCommit(const proto::Commit &commit) {
    if (!commit_quorum.AddAndCheckForQuorum(
            commit.primary_ui(), commit.replica_id(), commit)) {
        return;
    }

    LogEntry *entry = log.Find(commit.primary_ui());
    if (entry == nullptr) {
        NOT_IMPLEMENTED(); // state transfer
    }
    entry->state = LOG_STATE_COMMITTED;

    // execution
    while (auto *commit_entry = log.Find(commit_number + 1)) {
        if (commit_entry->state == LOG_STATE_NOOP) {
            commit_number += 1;
            continue;
        }
        if (commit_entry->state != LOG_STATE_COMMITTED) {
            break;
        }

        commit_number += 1;

        struct ExecuteContext {
            string result;
            void set_reply(const string &result) { this->result = result; }
        };
        ExecuteContext ctx;
        Execute(commit_number, commit_entry->request, ctx);

        proto::Reply reply;
        reply.set_replica_id(replicaIdx);
        reply.set_request_number(commit_entry->request.clientreqid());
        reply.set_result(ctx.result);

        auto iter = client_table.find(commit_entry->request.clientid());
        if (iter != client_table.end()) {
            iter->second.ready = true;
            iter->second.reply = reply;
            epilogue_list.push_back(
                [ //
                    this, escaping_remote = iter->second.remote->clone(),
                    reply //
            ]() mutable {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    PBMessage pb_reply(reply);
                    SignedAdapter signed_reply(pb_reply, identifier, false);
                    transport->SendMessage(this, *remote, signed_reply);
                });
        } else {
            RWarning(
                "Client entry missing: client id = %lu",
                commit_entry->request.clientid());
        }
    }
}

void MinBFTReplica::HandleRequest(
    const TransportAddress &remote, const Request &request,
    const string &signed_request //
) {
    auto iter = client_table.find(request.clientid());
    if (iter != client_table.end()) {
        if (iter->second.request_number > request.clientreqid()) {
            return;
        }
        if (iter->second.request_number == request.clientreqid()) {
            if (iter->second.ready) {
                proto::Reply reply(iter->second.reply);
                PBMessage pb_reply(reply);
                SignedAdapter signed_reply(pb_reply, identifier, false);
                transport->SendMessage(this, remote, signed_reply);
            }
            return;
        }
        iter->second.request_number = request.clientreqid();
        iter->second.ready = false;
    } else {
        ClientEntry entry;
        entry.remote = unique_ptr<TransportAddress>(remote.clone());
        entry.request_number = request.clientreqid();
        entry.ready = false;
        client_table.emplace(request.clientid(), move(entry));
    }

    if (configuration.GetLeaderIndex(view_number) != replicaIdx) {
        return;
    }

    log.Append(new LogEntry(
        viewstamp_t(view_number, log.LastOpnum() + 1), LOG_STATE_PREPARED,
        request));
    proto::UIMessage ui_message;
    auto &prepare = *ui_message.mutable_prepare();
    prepare.set_view_number(view_number);
    prepare.set_signed_request(signed_request);
    MinBFTAdapter minbft_layer(nullptr, identifier, true);
    epilogue_list.push_back([this, ui_message, minbft_layer]() mutable {
        PBMessage pb_layer(ui_message);
        minbft_layer.SetInner(&pb_layer);
        string buffer;
        buffer.resize(minbft_layer.SerializedSize());
        minbft_layer.Serialize(&buffer.front());
        proto::MinBFTMessage m;
        *m.mutable_ui_message() = buffer;
        transport->SendMessageToAll(this, PBMessage(m));
    });
    SendCommit(minbft_layer.GetUI());
    ConcludeEpilogue();
}

} // namespace minbft
} // namespace dsnet