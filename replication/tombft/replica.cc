#include "replication/tombft/replica.h"

#include "common/pbmessage.h"
#include "common/signedadapter.h"
#include "lib/message.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace tombft {

using std::move;
using std::string;
using std::unique_ptr;
using PBAdapter = PBMessage;

TOMBFTReplica::TOMBFTReplica(
    const Configuration &config, int replica_index, const string &identifier,
    int worker_thread_count, Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_index, true, transport, app),
      runner(worker_thread_count), identifier(identifier),
      start_number((uint32_t)-1), last_executed(0), session_number(0),
      log(false) //
{
    transport->ListenOnMulticast(this, config);
}

static int n_message = 0, n_signed_message = 0;

void TOMBFTReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size //
) {
    runner.RunPrologue(
        [ //
            this, escaping_remote = remote.clone(),
            owned_buffer = string((const char *)buf, size),
            escaping_message = new proto::Message //
    ]() -> Runner::Solo {
            auto message = unique_ptr<proto::Message>(escaping_message);
            PBAdapter pb(*message);
            auto security =
                unique_ptr<SignedAdapter>(new SignedAdapter(pb, ""));
            auto tom =
                unique_ptr<TOMBFTAdapter>(new TOMBFTAdapter(*security, true));
            tom->Parse(owned_buffer.data(), owned_buffer.size());
            if (!tom->IsVerified() || !security->IsVerified()) {
                NOT_IMPLEMENTED();
                return nullptr;
            }
            switch (message->get_case()) {
            case proto::Message::GetCase::kRequest:
                return [ //
                           this, escaping_remote,
                           escaping_message = message.release(),
                           escaping_tom = tom.release()]() {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    auto message = unique_ptr<proto::Message>(escaping_message);
                    auto tom = unique_ptr<TOMBFTAdapter>(escaping_tom);
                    HandleRequest(*remote, *message->mutable_request(), *tom);
                    ConcludeEpilogue();
                };
            default:
                RPanic("Unexpected message case: %d", message->get_case());
            }

            return nullptr;
        });

    // smartNIC simulator
    // static string nic_message;
    // if (nic_message.data() == buf) { // this is a nic message
    //     nic_message.clear(); // to select next message as next nic message
    // } else if (nic_message.empty()) {
    //     nic_message = string((const char *)buf, size);
    //     char sig[] = "smartNIC";
    //     nic_message.replace(16, sizeof(sig) - 1, sig);
    //     // normally delay time is shorter than 1ms, but resolution limits
    //     transport->Timer(1, [this, escaping_remote = remote.clone()]()
    //     mutable {
    //         auto remote = unique_ptr<TransportAddress>(escaping_remote);
    //         ReceiveMessage(*remote, &nic_message.front(),
    //         nic_message.size());
    //     });
    // }
}

TOMBFTReplica::~TOMBFTReplica() {
    RNotice(
        "n_message = %d, n_signed_message = %d", n_message, n_signed_message);
    RNotice(
        "average batch size = %d", (n_message + 1) / (n_signed_message + 1));
}

void TOMBFTReplica::HandleRequest(
    TransportAddress &remote, Request &message, TOMBFTAdapter &meta //
) {
    if (!address_table[message.clientid()]) {
        address_table[message.clientid()] =
            unique_ptr<TransportAddress>(remote.clone());
    }

    // convinent hack
    if (session_number == 0) {
        session_number = meta.SessionNumber();
    }
    if (start_number > meta.MessageNumber()) {
        start_number = meta.MessageNumber();
    }
    // RNotice(
    //     "message number = %u, is signed = %d", meta.MessageNumber(),
    //     meta.IsSigned());

    if (session_number != meta.SessionNumber()) {
        RWarning(
            "Session changed: %u -> %u", session_number, meta.SessionNumber());
        NOT_IMPLEMENTED();
    }

    if (meta.MessageNumber() < start_number + last_executed) {
        return; // either it is signed or not, it is useless
    }

    n_message += 1;
    // TODO save all message (with duplicated message number) for BFT
    // RNotice("buffering %d", meta.MessageNumber());
    tom_buffer[meta.MessageNumber()] = message;

    if (meta.IsSigned()) {
        n_signed_message += 1;
        auto iter = tom_buffer.begin();
        while (iter != tom_buffer.end()) {
            if (iter->first > meta.MessageNumber()) {
                break;
            }
            // RNotice("executing %d", iter->first);
            if (iter->first != start_number + last_executed) {
                RPanic(
                    "Gap: %lu (+%lu)", start_number + last_executed,
                    iter->first - (start_number + last_executed));
                NOT_IMPLEMENTED(); // state transfer
            }
            ExecuteOne(iter->second);
            iter = tom_buffer.erase(iter);
        }
        return;
    }
}

void TOMBFTHMACReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size //
) {
    runner.RunPrologue(
        [ //
            this, escaping_remote = remote.clone(),
            owned_buffer = string((const char *)buf, size),
            escaping_message = new proto::Message //
    ]() -> Runner::Solo {
            auto message = unique_ptr<proto::Message>(escaping_message);
            PBAdapter pb(*message);
            auto security =
                unique_ptr<SignedAdapter>(new SignedAdapter(pb, ""));
            auto tom = unique_ptr<TOMBFTHMACAdapter>(
                new TOMBFTHMACAdapter(*security, replicaIdx, true));
            tom->Parse(owned_buffer.data(), owned_buffer.size());
            if (!tom->IsVerified() || !security->IsVerified()) {
                NOT_IMPLEMENTED();
                return nullptr;
            }
            switch (message->get_case()) {
            case proto::Message::GetCase::kRequest:
                return [ //
                           this, escaping_remote,
                           escaping_message = message.release(),
                           escaping_tom = tom.release()]() {
                    auto remote = unique_ptr<TransportAddress>(escaping_remote);
                    auto message = unique_ptr<proto::Message>(escaping_message);
                    auto tom = unique_ptr<TOMBFTHMACAdapter>(escaping_tom);
                    HandleHMACRequest(
                        *remote, *message->mutable_request(), *tom);
                    ConcludeEpilogue();
                };
            default:
                RPanic("Unexpected message case: %d", message->get_case());
            }

            return nullptr;
        });
}

void TOMBFTHMACReplica::HandleHMACRequest(
    TransportAddress &remote, Request &message, TOMBFTHMACAdapter &meta //
) {
    if (!address_table[message.clientid()]) {
        address_table[message.clientid()] =
            unique_ptr<TransportAddress>(remote.clone());
    }

    // convinent hack
    if (session_number == 0) {
        session_number = meta.SessionNumber();
    }
    if (start_number > meta.MessageNumber()) {
        start_number = meta.MessageNumber();
    }
    // RNotice(
    //     "message number = %u, is signed = %d", meta.MessageNumber(),
    //     meta.IsSigned());

    if (session_number != meta.SessionNumber()) {
        RWarning(
            "Session changed: %u -> %u", session_number, meta.SessionNumber());
        NOT_IMPLEMENTED();
    }

    if (meta.MessageNumber() < start_number + last_executed) {
        return; // either it is signed or not, it is useless
    }

    if (meta.MessageNumber() != start_number + last_executed) {
        NOT_IMPLEMENTED(); // state transfer
    }

    ExecuteOne(message);
}

struct TOMEntry : public LogEntry {
    TOMEntry(viewstamp_t viewstamp, LogEntryState state, const Request &request)
        : LogEntry(viewstamp, state, request) {}
};

struct ExecuteContext {
    string result;
    void set_reply(const string &result) { this->result = result; }
};

void TOMBFTReplica::ExecuteOne(Request &request_message) {
    last_executed += 1;
    // TODO
    log.Append(new TOMEntry(
        viewstamp_t(0, last_executed), LOG_STATE_SPECULATIVE, request_message));

    auto message = unique_ptr<proto::Message>(new proto::Message);
    if (client_table.count(request_message.clientid())) {
        if (client_table[request_message.clientid()].client_request() >
            request_message.clientreqid()) {
            return;
        }
        if (client_table[request_message.clientid()].client_request() ==
            request_message.clientreqid()) {
            *message->mutable_reply() =
                client_table[request_message.clientid()];
        }
    }
    if (!message->has_reply()) {
        ExecuteContext ctx;
        Execute(last_executed, request_message, ctx);
        auto reply = message->mutable_reply();
        reply->set_client_request(request_message.clientreqid());
        reply->set_result(ctx.result);
        reply->set_replica_index(replicaIdx);
        client_table[request_message.clientid()] = *reply;
    }

    epilogue_list.push_back(
        [ //
            this,
            escaping_remote =
                address_table[request_message.clientid()]->clone(),
            escaping_message = message.release() //
    ]() {
            auto message = unique_ptr<proto::Message>(escaping_message);
            auto remote = unique_ptr<TransportAddress>(escaping_remote);
            PBAdapter pb_layer(*message);
            transport->SendMessage(
                this, *remote, SignedAdapter(pb_layer, identifier, false));
        });
}

} // namespace tombft
} // namespace dsnet