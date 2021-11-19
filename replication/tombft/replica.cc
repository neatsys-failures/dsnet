#include "replication/tombft/replica.h"

#include "lib/message.h"
#include "common/signedadapter.h"
#include "common/pbmessage.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace tombft {

using std::string;
using std::unique_ptr;
using std::move;
using PBAdapter = PBMessage;

TOMBFTReplica::TOMBFTReplica(
    const Configuration &config, int replica_index, const string &identifier,
    int worker_thread_count, Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_index, true, transport, app),
    runner(worker_thread_count), identifier(identifier),
    last_message_number(0), last_executed(0), session_number(0), log(false)
{
    transport->ListenOnMulticast(this, config);
    // RDebug("System start");
}

void TOMBFTReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size) 
{
    // RDebug("Get packet");
    runner.RunPrologue([
        this,
        escaping_remote = remote.clone(),
        owned_buffer = string((const char *) buf, size)
    ]() -> Runner::Solo {
        auto message = unique_ptr<proto::Message>(new proto::Message);
        PBAdapter pb(*message);
        auto security = unique_ptr<SignedAdapter>(new SignedAdapter(pb, ""));  // TODO
        auto tom = unique_ptr<TOMBFTAdapter>(new TOMBFTAdapter(*security, false));
        tom->Parse(owned_buffer.data(), owned_buffer.size());
        if (!tom->IsVerified() || !security->IsVerified()) {
            //
            NOT_IMPLEMENTED();
            return nullptr;
        }
        switch (message->get_case()) {
        case proto::Message::GetCase::kRequest:
            return [
                this,
                escaping_remote,
                escaping_message = message.release(),
                escaping_tom = tom.release()
            ]() {
                auto remote = unique_ptr<TransportAddress>(escaping_remote);
                auto message = unique_ptr<proto::Message>(escaping_message);
                auto tom = unique_ptr<TOMBFTAdapter>(escaping_tom);
                HandleRequest(*remote, *message->mutable_request(), *tom);
            };
        default:
            RPanic("Unexpected message case: %d", message->get_case());
        }

        return nullptr;
    });
}

void TOMBFTReplica::HandleRequest(
    TransportAddress &remote, Request &message, TOMBFTAdapter &meta)
{
    if (session_number == 0) {
        session_number = meta.SessionNumber();
    }
    if (last_message_number == 0) {
        last_message_number = meta.MessageNumber() - 1;
    }

    if (session_number != meta.SessionNumber()) {
        RWarning("Session changed: %u -> %u", session_number, meta.SessionNumber());
        NOT_IMPLEMENTED();
        return;
    }
    if (meta.MessageNumber() <= last_message_number) {
        RWarning("Ignore duplicated request: message number = %u", meta.MessageNumber());
        return;
    }
    if (meta.MessageNumber() > last_message_number + 1) {
        RWarning("Gap detected: %u..<%u", last_message_number + 1, meta.MessageNumber());
        NOT_IMPLEMENTED();
        return;
    }

    last_message_number += 1;
    // TODO log
    ExecuteOne(message);
}

struct TOMEntry: public LogEntry {
    TOMEntry(viewstamp_t viewstamp, LogEntryState state, const Request &request)
        : LogEntry(viewstamp, state, request)
    {
    }
};

struct ExecuteContext {
    string result;
    void set_reply(const string &result) {
        this->result = result;
    }
};

void TOMBFTReplica::ExecuteOne(Request &request_message) {
    last_executed += 1;
    // TODO
    log.Append(new TOMEntry(viewstamp_t(0, last_executed), LOG_STATE_EXECUTED, request_message));

    auto message = unique_ptr<proto::Message>(new proto::Message);
    if (client_table.count(request_message.clientid())) {
        if (client_table[request_message.clientid()].client_request() > request_message.clientreqid()) {
            return;
        }
        if (client_table[request_message.clientid()].client_request() == request_message.clientreqid()) {
            *message->mutable_reply() = client_table[request_message.clientid()];
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

    runner.RunEpilogue([
        this,
        client_address = request_message.clientaddr(),
        escaping_message = message.release()
    ]() {
        auto message = unique_ptr<proto::Message>(escaping_message);
        // reuse `remote` from `HandleRequest` when possible?
        RDebug("remote = %s", ReplicaAddress(client_address).Serialize().c_str());
        auto remote = unique_ptr<TransportAddress>(
            transport->LookupAddress(ReplicaAddress(client_address)));
        PBAdapter pb_layer(*message);
        transport->SendMessage(this, *remote, SignedAdapter(pb_layer, "Alex"));
    });
}

}
}