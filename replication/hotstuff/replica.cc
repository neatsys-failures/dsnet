#include "replication/hotstuff/replica.h"
#include "common/signedadapter.h"
#include "lib/assert.h"
#include <cstdlib>

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace hotstuff {

using std::move;
using std::string;
using std::unique_ptr;

HotStuffReplica::HotStuffReplica(
    const Configuration &config, int index, string identifier, int n_thread,
    int batch_size, Transport *transport, AppReplica *app)
    : Replica(config, 0, index, false, transport, app), identifier(identifier),
      runner(n_thread), batch_size(batch_size), log(false) //
{
    setenv("DEBUG", "replica.cc", 1);

    resend_vote_timeout =
        unique_ptr<Timeout>(new Timeout(transport, 1000, [this]() {
            runner.RunPrologue([this]() {
                return [this]() {
                    if (IsPrimary()) {
                        NOT_REACHABLE();
                    }
                    RWarning("Resend VoteMessage");
                    if (generic_qc) {
                        SendVote(generic_qc->op_number());
                    } else {
                        SendVote(0);
                    }
                };
            });
        }));
    if (IsPrimary()) {
        send_generic_timeout =
            unique_ptr<Timeout>(new Timeout(transport, 200, [this]() {
                runner.RunPrologue([this]() {
                    return [this]() {
                        if (!IsPrimary()) {
                            NOT_REACHABLE();
                        }
                        RNotice("Send generic on timeout");
                        SendGeneric();
                    };
                });
            }));
        send_generic_timeout->Start();
    }

    runner.RunPrologue([this]() {
        return [this]() {
            if (!IsPrimary()) {
                SendVote(0);
            } else {
                ResetPendingGeneric();
            }
        };
    });
}

HotStuffReplica::~HotStuffReplica() {}

void HotStuffReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t length //
) {
    runner.RunPrologue(
        [this, owned_buffer = string((const char *)buf, length),
         escaping_remote = remote.clone()]() -> Runner::Solo {
            proto::Message message;
            PBMessage pb_layer(message);
            SignedAdapter signed_layer(pb_layer, "");
            signed_layer.Parse(owned_buffer.data(), owned_buffer.size());
            if (!signed_layer.IsVerified()) {
                RWarning("Received message fail to be verified");
                return nullptr;
            }

            switch (message.get_case()) {
            case proto::Message::GetCase::kRequest:
                return [this, escaping_remote, message]() {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    HandleRequest(*remote, message.request());
                };
            case proto::Message::GetCase::kGeneric: {
                auto &justify = message.generic().block().justify();
                // TODO check vote count, check vote from different backups
                for (int i = 0; i < justify.signed_vote_size(); i += 1) {
                    auto &signed_vote_buf = justify.signed_vote(i);
                    proto::VoteMessage vote_message;
                    PBMessage pb_vote(vote_message);
                    SignedAdapter signed_vote(pb_vote, "");
                    signed_vote.Parse(
                        signed_vote_buf.data(), signed_vote_buf.size());
                    if (!signed_vote.IsVerified()) {
                        RWarning("Generic message fail to verify QC");
                        return nullptr;
                    }
                }
                return [this, escaping_remote, message]() {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    HandleGeneric(*remote, message.generic());
                };
            }
            case proto::Message::GetCase::kVote:
                return [this, escaping_remote, message, owned_buffer]() {
                    auto remote =
                        unique_ptr<const TransportAddress>(escaping_remote);
                    HandleVote(*remote, message.vote(), owned_buffer);
                };
            default:
                RPanic("Unexpected message case: %d", message.get_case());
            }
        });
}

void HotStuffReplica::HandleRequest(
    const TransportAddress &remote, const Request &request //
) {
    auto iter = client_table.find(request.clientid());
    if (iter != client_table.end()) {
        auto &entry = iter->second;
        if (entry.client_request > request.clientreqid()) {
            return;
        }
        if (entry.client_request == request.clientreqid()) {
            if (entry.has_reply) {
                proto::Message reply;
                *reply.mutable_reply() = entry.reply_message;
                transport->SendMessage(this, remote, PBMessage(reply));
            }
            return;
        }
        entry.client_request = request.clientreqid();
        entry.has_reply = false;
    } else {
        client_table.emplace(
            request.clientid(), ClientEntry(remote, request.clientreqid()));
    }

    if (IsPrimary()) {
        opnum_t op_number = pending_generic->block().op_number() +
                            pending_generic->block().request_size() + 1;
        RDebug("Assign request: op number = %lu", op_number);
        log.Append(new HotStuffEntry(op_number, request));

        *pending_generic->mutable_block()->add_request() = request;
        if (pending_generic->block().request_size() >= batch_size) {
            SendGeneric();
        }
    }
}

void HotStuffReplica::HandleVote(
    const TransportAddress &remote, const proto::VoteMessage &vote,
    const SignedVote &signed_vote //
) {
    if (!IsPrimary()) {
        NOT_REACHABLE();
    }
    if (generic_qc && generic_qc->op_number() >= vote.op_number()) {
        return;
    }

    // RDebug(
    //     "vote: op_number = %lu, replic id = %d", vote.op_number(),
    //     vote.replica_index());
    high_qc[vote.op_number()][vote.replica_index()] = signed_vote;
    // collect 2f from backups, then add one from self
    if (high_qc[vote.op_number()].size() >= 2 * configuration.f) {
        RDebug("New QC collected: op_number = %lu", vote.op_number());
        proto::QC qc;
        qc.set_op_number(vote.op_number());
        for (const auto &iter : high_qc[vote.op_number()]) {
            qc.add_signed_vote(iter.second);
        }

        proto::VoteMessage vote_message;
        vote_message.set_op_number(vote.op_number());
        vote_message.set_replica_index(replicaIdx);
        PBMessage pb_vote(vote_message);
        SignedAdapter signed_vote(pb_vote, identifier);
        string signed_vote_buf;
        signed_vote_buf.resize(signed_vote.SerializedSize());
        signed_vote.Serialize(&signed_vote_buf.front());
        qc.add_signed_vote(signed_vote_buf);

        EnterNextView(qc);
    }
}

void HotStuffReplica::HandleGeneric(
    const TransportAddress &remote, const proto::GenericMessage &generic //
) {
    if (IsPrimary()) {
        NOT_REACHABLE();
    }

    SendVote(generic.block().op_number());

    opnum_t op_offset = generic.block().op_number();
    RDebug(
        "Generic block: op number = %lu (+%u)", op_offset,
        generic.block().request_size());
    if (op_offset != log.LastOpnum() + 1) {
        NOT_IMPLEMENTED(); // state transfer
    }
    log.Append(new HotStuffEntry(op_offset));
    for (opnum_t i = 0; i < generic.block().request_size(); i += 1) {
        opnum_t op_number = op_offset + i + 1;
        log.Append(new HotStuffEntry(op_number, generic.block().request(i)));
    }
    if ( //
        !generic_qc ||
        generic.block().justify().op_number() >= generic_qc->op_number() //
    ) {
        EnterNextView(generic.block().justify());
    }
}

struct ExecuteContext {
    string result;
    void set_reply(const string &result) { this->result = result; }
};

void HotStuffReplica::EnterNextView(const proto::QC &justify) {
    RDebug("Enter new view: justified op_number = %lu", justify.op_number());
    if (justify.op_number() != 0) {
        log.Find(justify.op_number())->As<HotStuffEntry>().justify = justify;
    }

    auto commit_qc = move(locked_qc);
    locked_qc = move(generic_qc);
    generic_qc = unique_ptr<proto::QC>(new proto::QC(justify));

    // if (IsPrimary()) {
    //     // sending GenericMessage unconditionally to simply ensure liveness
    //     of
    //     // last few requests
    //     // to reduce overhead, it is able to skip this sending and add close
    //     // batch timeout
    //     SendGeneric();
    // }

    if (!commit_qc || commit_qc->op_number() == 0) {
        return;
    }

    for ( //
        opnum_t op_number = commit_qc->op_number();
        op_number < locked_qc->op_number(); op_number += 1 //
    ) {
        HotStuffEntry &entry = log.Find(op_number)->As<HotStuffEntry>();
        if (entry.state == LOG_STATE_NOOP) {
            continue;
        }
        entry.state = LOG_STATE_COMMITTED;

        ExecuteContext ctx;
        Execute(op_number, entry.request, ctx);

        proto::ReplyMessage message;
        message.set_client_request(entry.request.clientreqid());
        message.set_result(ctx.result);
        message.set_replica_index(replicaIdx);

        const auto &iter = client_table.find(entry.request.clientid());
        if (iter != client_table.end()) { // almost always
            const TransportAddress &remote = *iter->second.remote;
            // Do this in epilogue?
            transport->SendMessage(this, remote, PBMessage(message));

            iter->second.reply_message = message;
            iter->second.has_reply = true;
        } else {
            RWarning("Client entry not found, skip send reply");
        }
    }
}

void HotStuffReplica::SendGeneric() {
    if (!IsPrimary()) {
        NOT_REACHABLE();
    }
    send_generic_timeout->Reset();

    if (!generic_qc) {
        RWarning("Generic QC not present, skip send GenericMessage");
        return;
    }
    *pending_generic->mutable_block()->mutable_justify() = *generic_qc;
    RDebug(
        "SendGeneric: block op number = %lu (+%u), justify op number = %lu",
        pending_generic->block().op_number(),
        pending_generic->block().request_size(),
        pending_generic->block().justify().op_number());

    runner.RunEpilogue([this, escaping_generic = pending_generic.release()]() {
        auto generic = unique_ptr<proto::GenericMessage>(escaping_generic);
        proto::Message message;
        *message.mutable_generic() = *generic;
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier);
        transport->SendMessageToAll(this, signed_layer);
    });

    ResetPendingGeneric();
}

void HotStuffReplica::ResetPendingGeneric() {
    pending_generic =
        unique_ptr<proto::GenericMessage>(new proto::GenericMessage);
    RDebug("Reset pending generic: op number = %lu", log.LastOpnum() + 1);
    pending_generic->mutable_block()->set_op_number(log.LastOpnum() + 1);
    log.Append(new HotStuffEntry(log.LastOpnum() + 1));
}

void HotStuffReplica::SendVote(opnum_t op_number) {
    runner.RunEpilogue([this, op_number]() {
        proto::Message message;
        auto &vote = *message.mutable_vote();
        vote.set_op_number(op_number);
        vote.set_replica_index(replicaIdx);
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier);
        transport->SendMessageToReplica(this, GetPrimary(), signed_layer);
    });
    resend_vote_timeout->Reset();
}

} // namespace hotstuff
} // namespace dsnet
