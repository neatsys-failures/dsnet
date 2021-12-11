#pragma once
#include "common/log.h"
#include "common/pbmessage.h"
#include "common/replica.h"
#include "common/runner.h"
#include "common/signedadapter.h"
#include "lib/transport.h"
#include "replication/tombft/adapter.h"
#include "replication/tombft/message.pb.h"

namespace dsnet {
namespace tombft {

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

template <typename Layout> class TOMBFTReplicaCommon : public Replica {
public:
    TOMBFTReplicaCommon(
        const Configuration &config, int replica_index,
        const string &identifier, int worker_thread_count, Transport *transport,
        AppReplica *app);

    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t len) override;

protected:
    CTPLOrderedRunner runner;
    const string identifier;

    uint32_t start_number;
    uint64_t last_executed;
    uint8_t session_number;

    Log log;
    std::unordered_map<uint64_t, proto::ReplyMessage> client_table;
    std::unordered_map<uint64_t, std::unique_ptr<TransportAddress>>
        address_table;

    std::map<uint32_t, Request> tom_buffer;

    virtual void
    HandleRequest(TransportAddress &remote, Request &message, Layout &meta) = 0;

    void ExecuteOne(Request &message); // insert into log by the way

    std::vector<Runner::Epilogue> epilogue_list;
    void ConcludeEpilogue() {
        if (epilogue_list.empty()) {
            return;
        }
        std::vector<Runner::Epilogue> runner_list(epilogue_list);
        epilogue_list.clear();

        runner.RunEpilogue([runner_list] {
            for (Runner::Epilogue epilogue : runner_list) {
                epilogue();
            }
        });
    }
};

template <typename Layout>
TOMBFTReplicaCommon<Layout>::TOMBFTReplicaCommon(
    const Configuration &config, int replica_index, const string &identifier,
    int worker_thread_count, Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_index, true, transport, app),
      runner(worker_thread_count),
      identifier(identifier), start_number((uint32_t)-1), last_executed(0),
      session_number(0), log(false) //
{
    transport->ListenOnMulticast(this, config);
}

template <typename Layout>
void TOMBFTReplicaCommon<Layout>::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size //
) {
    runner.RunPrologue(
        [ //
            this, escaping_remote = remote.clone(),
            owned_buffer = string((const char *)buf, size) //
    ]() -> Runner::Solo {
            proto::Message message;
            PBMessage pb(message);
            SignedAdapter security(pb, "");
            Layout tom(security, true, replicaIdx);
            tom.Parse(owned_buffer.data(), owned_buffer.size());
            if (!tom.IsVerified() || !security.IsVerified()) {
                RWarning("Failed to verify TOM packet");
                return nullptr;
            }
            switch (message.get_case()) {
            case proto::Message::GetCase::kRequest:
                return [ //
                           this, escaping_remote, message, tom]() mutable {
                    auto remote =
                        std::unique_ptr<TransportAddress>(escaping_remote);
                    HandleRequest(*remote, *message.mutable_request(), tom);
                    ConcludeEpilogue();
                };
            default:
                RPanic("Unexpected message case: %d", message.get_case());
            }

            return nullptr;
        });

    // smartNIC simulator
    
    static string nic_message;
    if (nic_message.data() == buf) { // this is a nic message
        nic_message.clear(); // to select next message as next nic message
    } else if (nic_message.empty()) {
        nic_message = string((const char *)buf, size);
        char sig[] = "smartNIC";
        nic_message.replace(16, sizeof(sig) - 1, sig);
        // normally delay time is shorter than 1ms, but resolution limits
        transport->Timer(1, [this, escaping_remote = remote.clone()]()
        mutable {
            auto remote = std::unique_ptr<TransportAddress>(escaping_remote);
            ReceiveMessage(*remote, &nic_message.front(),
            nic_message.size());
        });
    }
}

struct TOMEntry : public LogEntry {
    TOMEntry(viewstamp_t viewstamp, LogEntryState state, const Request &request)
        : LogEntry(viewstamp, state, request) {}
};

template <typename Layout>
void TOMBFTReplicaCommon<Layout>::ExecuteOne(Request &request_message) {
    last_executed += 1;
    // TODO
    log.Append(new TOMEntry(
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

class TOMBFTReplica : public TOMBFTReplicaCommon<TOMBFTAdapter> {
    void HandleRequest(
        TransportAddress &remote, Request &message,
        TOMBFTAdapter &meta) override;

public:
    TOMBFTReplica(
        const Configuration &config, int replica_index,
        const string &identifier, int worker_thread_count, Transport *transport,
        AppReplica *app)
        : TOMBFTReplicaCommon<TOMBFTAdapter>(
              config, replica_index, identifier, worker_thread_count, transport,
              app) {}
    ~TOMBFTReplica();
};

class TOMBFTHMACReplica : public TOMBFTReplicaCommon<TOMBFTHMACAdapter> {
    void HandleRequest(
        TransportAddress &remote, Request &message,
        TOMBFTHMACAdapter &meta) override;

public:
    TOMBFTHMACReplica(
        const Configuration &config, int replica_index,
        const string &identifier, int worker_thread_count, Transport *transport,
        AppReplica *app)
        : TOMBFTReplicaCommon<TOMBFTHMACAdapter>(
              config, replica_index, identifier, worker_thread_count, transport,
              app) {}
};

#undef RPanic
#undef RDebug
#undef RNotice
#undef RWarning

} // namespace tombft
} // namespace dsnet