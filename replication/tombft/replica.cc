#include "replication/tombft/replica.h"

#include "common/pbmessage.h"
#include "common/signedadapter.h"
#include "lib/latency.h"
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

static int n_message = 0, n_signed_message = 0, n_query = 0;

TOMBFTReplicaBase::TOMBFTReplicaBase(
    const Configuration &config, int replica_index, const string &identifier,
    Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_index, true, transport, app),
      identifier(identifier), last_executed(0), session_number(0), log(false) //
{
    transport->ListenOnMulticast(this, config);
    status = STATUS_NORMAL;

    query_timeout = unique_ptr<Timeout>(new Timeout(transport, 1, [this] {
        GetRunner().RunPrologue([this] {
            return [this] {
                StartQuery(next_query_number, false);
                ConcludeEpilogue();
            };
        });
    }));
    queried_message_number = 0;
    next_query_number = 0;
}

void TOMBFTReplicaBase::ExecuteOne(const Request &request_message) {
    last_executed += 1;
    // TODO
    log.Append(new LogEntry(
        viewstamp_t(0, last_executed), LOG_STATE_SPECULATIVE, request_message));

    auto message = std::unique_ptr<proto::Message>(new proto::Message);
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
        struct ExecuteContext {
            string result;
            void set_reply(const string &result) { this->result = result; }
        };
        ExecuteContext ctx;
        Execute(last_executed, request_message, ctx);
        auto reply = message->mutable_reply();
        reply->set_client_request(request_message.clientreqid());
        reply->set_result(ctx.result);
        reply->set_replica_index(replicaIdx);
        client_table[request_message.clientid()] = *reply;
    }

    if (!address_table[request_message.clientid()]) {
        RWarning("Skip sending reply");
        return;
    }

    epilogue_list.push_back(
        [ //
            this,
            escaping_remote =
                address_table[request_message.clientid()]->clone(),
            escaping_message = message.release() //
    ]() {
            auto message = std::unique_ptr<proto::Message>(escaping_message);
            auto remote = std::unique_ptr<TransportAddress>(escaping_remote);
            PBMessage pb_layer(*message);
            transport->SendMessage(
                this, *remote, SignedAdapter(pb_layer, identifier, false));
        });
}

void TOMBFTReplicaBase::TryExecute() {
    auto iter = tom_buffer.begin();
    while (iter != tom_buffer.end()) {
        if (iter->first != last_executed + offset + 1) {
            if (next_query_number != last_executed + offset + 1) {
                next_query_number = last_executed + offset + 1;
                query_timeout->Start();
                // shortcut to start query sooner
                // if (tom_buffer.size() > 8) {
                //     StartQuery(next_query_number);
                // }
            }
            break;
        }
        if (iter->first > high_verified) {
            break;
        }
        ExecuteOne(iter->second);
        iter = tom_buffer.erase(iter);
    }

    if (last_executed + offset >= next_query_number) {
        query_timeout->Stop();
    }
}

DEFINE_LATENCY(signed_delay);

TOMBFTReplicaBase::~TOMBFTReplicaBase() {
    RNotice(
        "n_message = %d, n_signed_message = %d", n_message, n_signed_message);
    RNotice(
        "average batch size = %d", (n_message + 1) / (n_signed_message + 1));
    RNotice("n_query = %d", n_query);

    Latency_Dump(&signed_delay);
}

void TOMBFTReplicaBase::StartEpochChange(sessnum_t next_session_number) {
    status = STATUS_EPOCH_CHANGE;
    RNotice(
        "Start epoch change: %lu -> %lu", session_number, next_session_number);

    // assume that old session long gone, all gap resolved, etc
    tom_buffer.clear();

    proto::Message message;
    auto &view_change = *message.mutable_view_change();
    view_change.set_view_number(session_number);
    view_change.set_next_view_number(next_session_number);
    view_change.set_high_message_number(last_executed);
    view_change.set_replica_index(replicaIdx);
    epilogue_list.push_back([this, message]() mutable {
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier);
        // this is bad, should generalize
        TOMBFTHMACAdapter tom_layer(signed_layer, false, replicaIdx);
        // TODO no hardcoded leader
        transport->SendMessageToReplica(this, 0, tom_layer);
    });
    if (replicaIdx == 0) {
        InsertViewChange(view_change);
    }
}

void TOMBFTReplicaBase::HandleViewChange(
    const TransportAddress &remote, const proto::ViewChange &view_change //
) {
    if (status != STATUS_EPOCH_CHANGE) {
        return;
    }

    // TODO check view number, next view number, leadership
    InsertViewChange(view_change);
}

void TOMBFTReplicaBase::InsertViewChange(const proto::ViewChange &view_change) {
    view_change_set.emplace(view_change.replica_index(), view_change);
    if ((int)view_change_set.size() < configuration.f * 2 + 1) {
        return;
    }

    RNotice("Start view: session number = %u", view_change.next_view_number());

    // on fast path, assume everyone has even high message number
    // no message sync in ViewStart

    proto::Message message;
    auto &view_start = *message.mutable_view_start();
    view_start.set_view_number(view_change.next_view_number());
    view_start.set_high_message_number(last_executed);
    epilogue_list.push_back([this, message]() mutable {
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier);
        TOMBFTHMACAdapter tom_layer(signed_layer, false, replicaIdx);
        transport->SendMessageToAll(this, tom_layer);
    });

    // assume epock change here
    SendEpochStart(view_change.next_view_number());
}

void TOMBFTReplicaBase::HandleViewStart(
    const TransportAddress &remote, const proto::ViewStart &view_start //
) {
    if (status != STATUS_EPOCH_CHANGE) {
        return;
    }

    // assume identical log, skip log install
    RNotice("Start view: session number = %u", view_start.view_number());

    SendEpochStart(view_start.view_number());
}

void TOMBFTReplicaBase::SendEpochStart(sessnum_t session_number) {
    RNotice("Send epoch start: session number = %lu", session_number);

    proto::Message message;
    auto &epoch_start = *message.mutable_epoch_start();
    epoch_start.set_session_number(session_number);
    epoch_start.set_high_message_number(last_executed);
    epoch_start.set_replica_index(replicaIdx);
    epilogue_list.push_back([this, message]() mutable {
        PBMessage pb_layer(message);
        SignedAdapter signed_layer(pb_layer, identifier);
        TOMBFTHMACAdapter tom_layer(signed_layer, false, replicaIdx);
        transport->SendMessageToAll(this, tom_layer);
    });
    InsertEpochStart(epoch_start);
}

void TOMBFTReplicaBase::HandleEpochStart(
    const TransportAddress &remote, const proto::EpochStart &epoch_start //
) {
    if (status != STATUS_EPOCH_CHANGE) {
        return;
    }
    // TODO check high op number, session number
    InsertEpochStart(epoch_start);
}

void TOMBFTReplicaBase::InsertEpochStart(const proto::EpochStart &epoch_start) {
    epoch_start_set.emplace(epoch_start.replica_index(), epoch_start);
    if ((int)epoch_start_set.size() < configuration.f * 2 + 1) {
        return;
    }

    RNotice("Epoch start: session number = %u", epoch_start.session_number());

    status = STATUS_NORMAL;
    view_change_set.clear();
    epoch_start_set.clear();
    session_number = epoch_start.session_number();
    offset = 0 - last_executed;

    TryExecute();
}

void TOMBFTReplicaBase::StartQuery(uint32_t message_number, bool spec) {
    // TODO initiate gap agreement instead of resend
    if (queried_message_number == message_number && spec) {
        return;
    }
    // RWarning("Send Query: message number = %u", message_number);
    queried_message_number = message_number;
    epilogue_list.push_back([this, message_number] {
        proto::Message m;
        auto &query = *m.mutable_query();
        query.set_replica_index(replicaIdx);
        query.set_message_number(message_number);
        PBMessage pb_layer(m);
        SignedAdapter signed_layer(pb_layer, identifier, false);
        TOMBFTHMACAdapter tom_layer(signed_layer, false, replicaIdx);
        // RWarning("Send Query (epilogue): message number = %u",
        // message_number);
        // if (tom_layer.SerializedSize() < 15) {
        //     NOT_REACHABLE();
        // }
        transport->SendMessageToAll(this, tom_layer);
    });
    n_query += 1;
}

void TOMBFTReplicaBase::HandleQueryMessage(
    const TransportAddress &remote, const proto::QueryMessage &query //
) {
    // RNotice(
    //     "HandleQuery: replica id = %d, message number = %u",
    //     query.replica_index(), query.message_number());
    if (!query_buffer.count(query.message_number())) {
        // RWarning("Ignore unseen queried");
        delayed_query_reply[query.message_number()] = [this, query] {
            epilogue_list.push_back([this, query] {
                const auto &message = query_buffer.at(query.message_number());
                transport->SendMessageToReplica(
                    this, query.replica_index(),
                    BufferMessage(message.data(), message.size()));
            });
        };
        return;
    }
    epilogue_list.push_back([ //
                                this, escaping_remote = remote.clone(),
                                message_number = query.message_number() //
    ] {
        const auto &message = query_buffer.at(message_number);

        if (message.size() < 20) {
            RPanic("Wrong message on query_buffer");
        }
        auto remote = unique_ptr<TransportAddress>(escaping_remote);
        // const auto &message = query_buffer[message_number];
        transport->SendMessage(
            this, *remote, BufferMessage(message.data(), message.size()));
    });
}

static uint32_t measured_number = 0;

void TOMBFTReplica::HandleRequest(
    const TransportAddress &remote, const Request &message,
    const TOMBFTAdapter &meta //
) {
    if (status != STATUS_NORMAL) {
        tom_buffer.emplace(meta.MessageNumber(), message);
        return;
    }

    if (!address_table[message.clientid()] &&
        meta.MessageNumber() != next_query_number) {
        address_table[message.clientid()] =
            unique_ptr<TransportAddress>(remote.clone());
    }

    // convinent hack on the very first packet
    if (session_number == 0) {
        session_number = meta.SessionNumber();
        offset = meta.MessageNumber() - 1;
    }
    // RNotice(
    //     "message number = %u, is signed = %d", meta.MessageNumber(),
    //     meta.IsSigned());

    if (session_number != meta.SessionNumber()) {
        NOT_IMPLEMENTED();
    }

    if (meta.MessageNumber() <= offset + last_executed) {
        return; // either it is signed or not, it is useless
    }

    n_message += 1;
    // TODO save all message (with duplicated message number) for BFT
    // RNotice("buffering %d", meta.MessageNumber());
    tom_buffer[meta.MessageNumber()] = message;

    if (meta.IsSigned() && meta.MessageNumber() > high_verified) {
        high_verified = meta.MessageNumber();
    }
    TryExecute();

    if (measured_number == 0) {
        Latency_Start(&signed_delay);
        measured_number = meta.MessageNumber();
    }
}

void TOMBFTHMACReplica::HandleRequest(
    const TransportAddress &remote, const Request &message,
    const TOMBFTHMACAdapter &meta //
) {
    if (status != STATUS_NORMAL) {
        tom_buffer.emplace(meta.MessageNumber(), message);
        return;
    }

    // convinent hack for the very first packet
    if (session_number == 0) {
        session_number = meta.SessionNumber();
        offset = meta.MessageNumber() - 1;
    }
    // RNotice(
    //     "message number = %u, is signed = %d", meta.MessageNumber(),
    //     meta.IsSigned());

    if (session_number != meta.SessionNumber()) {
        RWarning(
            "Session changed: %lu -> %u", session_number, meta.SessionNumber());

        StartEpochChange(meta.SessionNumber());
        // insert message after start epoch change, because buffer get cleared
        // inside that
        tom_buffer.emplace(meta.MessageNumber(), message);
        return;
    }

    if (meta.MessageNumber() <= offset + last_executed) {
        return; // either it is signed or not, it is useless
    }

    if (!address_table[message.clientid()] &&
        meta.MessageNumber() != next_query_number) {
        address_table[message.clientid()] =
            unique_ptr<TransportAddress>(remote.clone());
    }

    n_message += 1;
    if (meta.MessageNumber() != offset + last_executed + 1) {
        // RWarning(
        //     "Gap: %lu (+%lu)", offset + last_executed + 1,
        //     meta.MessageNumber() - (offset + last_executed));
        tom_buffer.emplace(meta.MessageNumber(), message);
    } else {
        ExecuteOne(message);
    }
    TryExecute();
}

} // namespace tombft
} // namespace dsnet