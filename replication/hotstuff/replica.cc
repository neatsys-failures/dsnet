#include "replication/hotstuff/replica.h"
#include "common/signedadapter.h"
#include "lib/assert.h"

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
    int batch_size_max, Transport *transport, AppReplica *app)
    : Replica(config, 0, index, false, transport, app), identifier(identifier),
      runner(n_thread), batch_size_max(batch_size_max), log(false) //
{
    //
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
                NOT_REACHABLE();
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
    } else {
        client_table.emplace(request.clientid(), ClientEntry(remote));
    }

    if (!IsPrimary()) {
        return;
    }

    pending_requests.push_back(request);
    // TODO propose
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

    high_qc[vote.op_number()][vote.replica_index()] = signed_vote;
    if (high_qc[vote.op_number()].size() >= 2 * configuration.f + 1) {
        proto::QC qc;
        qc.set_op_number(vote.op_number());
        for (const auto &iter : high_qc[vote.op_number()]) {
            qc.add_signed_vote(iter.second);
        }
        EnterNextView(qc);
    }
}

void HotStuffReplica::HandleGeneric(
    const TransportAddress &remote, const proto::GenericMessage &generic //
) {
    if (IsPrimary()) {
        NOT_REACHABLE();
    }

    opnum_t op_offset = generic.block().op_number();
    for (opnum_t i = 0; i < generic.block().request_size(); i += 1) {
        opnum_t op_number = op_offset + i;
        if (op_number != log.LastOpnum() + 1) {
            NOT_IMPLEMENTED(); // state transfer
        }
        log.Append(new HotStuffEntry(op_number, generic.block().request(i)));
    }
    if (generic.block().justify().op_number() >= generic_qc->op_number()) {
        EnterNextView(generic.block().justify());
    }
}

struct ExecuteContext {
    string result;
    void set_reply(const string &result) { this->result = result; }
};

void HotStuffReplica::EnterNextView(const proto::QC &justify) {
    log.Find(justify.op_number())->As<HotStuffEntry>().justify = justify;

    auto commit_qc = move(locked_qc);
    locked_qc = move(generic_qc);
    generic_qc = unique_ptr<proto::QC>(new proto::QC(justify));

    if (IsPrimary()) {
        // Send generic
    }

    if (!commit_qc) {
        return;
    }

    for ( //
        opnum_t op_number = commit_qc->op_number();
        op_number < locked_qc->op_number(); op_number += 1 //
    ) {
        HotStuffEntry &entry = log.Find(op_number)->As<HotStuffEntry>();
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
            transport->SendMessage(this, remote, PBMessage(message));
        }
    }
}

} // namespace hotstuff
} // namespace dsnet
