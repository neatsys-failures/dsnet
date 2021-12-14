#pragma once
#include "common/log.h"
#include "common/pbmessage.h"
#include "common/replica.h"
#include "common/runner.h"
#include "common/signedadapter.h"
#include "lib/transport.h"
#include "replication/tombft/adapter.h"
#include "replication/tombft/message.pb.h"
#include "sequencer/sequencer.h"
#include <deque>

namespace dsnet {
namespace tombft {

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

template <typename Layout> struct LayoutToRunner {};
template <> struct LayoutToRunner<TOMBFTAdapter> { using Runner = SpinRunner; };
template <> struct LayoutToRunner<TOMBFTHMACAdapter> {
    using Runner = SpinOrderedRunner;
};

template <typename BaseRunner> class TOMBFTRunner : public BaseRunner {
public:
    TOMBFTRunner(int n_worker, int replica_id) : BaseRunner(n_worker) {
        int &core_id = BaseRunner::core_id;
        if (replica_id / 4 == 0) {
            return;
        }
        switch (replica_id / 4) {
        case 1:
            core_id = 5;
            break;
        case 2:
            core_id = 10;
            break;
        case 3:
            core_id = 32;
            break;
        case 4:
            core_id = 37;
            break;
        case 5:
            core_id = 42;
            break;
        default:
            Panic("Too many replicas");
        }
        BaseRunner::SetAffinity();
        for (int i = 0; i < BaseRunner::NWorker(); i += 1) {
            BaseRunner::SetAffinity(BaseRunner::GetWorker(i));
        }
    }
};

class TOMBFTReplicaBase : public Replica {
protected:
    TOMBFTReplicaBase(
        const Configuration &config, int replica_index,
        const string &identifier, Transport *transport, AppReplica *app);
    ~TOMBFTReplicaBase();

    const string identifier;

    int64_t offset; // message number - log op number
    uint64_t last_executed, high_verified;
    sessnum_t session_number;

    std::unique_ptr<Timeout> query_timeout;

    Log log;
    std::unordered_map<uint64_t, proto::ReplyMessage> client_table;
    std::unordered_map<uint64_t, std::unique_ptr<TransportAddress>>
        address_table;

    void HandleViewChange(
        const TransportAddress &remote, const proto::ViewChange &view_change);
    std::unordered_map<int, proto::ViewChange> view_change_set;
    void InsertViewChange(const proto::ViewChange &view_change);
    void HandleViewStart(
        const TransportAddress &remote, const proto::ViewStart &view_start);
    void SendEpochStart(sessnum_t session_number);
    void HandleEpochStart(
        const TransportAddress &remote, const proto::EpochStart &epoch_start);
    void InsertEpochStart(const proto::EpochStart &epoch_start);
    std::unordered_map<int, proto::EpochStart> epoch_start_set;

    void StartQuery(uint32_t message_number);
    uint32_t queried_message_number, next_query_number;
    void HandleQueryMessage(
        const TransportAddress &remote, const proto::QueryMessage &query);
    std::unordered_map<uint32_t, std::string> query_buffer; // buffer for query

    // TODO should buffer for multiple view/session?
    std::map<uint32_t, Request> tom_buffer;
    void ExecuteOne(const Request &message); // insert into log by the way
    void TryExecute();

    std::vector<Runner::Epilogue> epilogue_list;
    void ConcludeEpilogue() {
        if (epilogue_list.empty()) {
            return;
        }
        GetRunner().RunEpilogue([runner_list = epilogue_list] {
            for (Runner::Epilogue epilogue : runner_list) {
                epilogue();
            }
        });
        epilogue_list.clear();
    }
    void StartEpochChange(sessnum_t next_session_number);

    virtual Runner &GetRunner() = 0;
};

template <typename Layout>
class TOMBFTReplicaCommon : public TOMBFTReplicaBase {
public:
    TOMBFTReplicaCommon(
        const Configuration &config, int replica_index,
        const string &identifier, int worker_thread_count, Transport *transport,
        AppReplica *app)
        : TOMBFTReplicaBase(config, replica_index, identifier, transport, app),
          runner(worker_thread_count, replica_index) {}

    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t len) override;

protected:
    TOMBFTRunner<typename LayoutToRunner<Layout>::Runner> runner;
    Runner &GetRunner() override { return runner; }

    virtual void HandleRequest(
        const TransportAddress &remote, const Request &message,
        const Layout &meta) = 0;
};

template <typename Layout>
void TOMBFTReplicaCommon<Layout>::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size //
) {
    runner.RunPrologue(
        [ //
            this, escaping_remote = remote.clone(),
            owned_buffer = string((const char *)buf, size) //
    ]() -> Runner::Solo {
            // RNotice("Receive");
            if (owned_buffer.size() <= 14) {
                RWarning(
                    "remote = %s, size = %lu",
                    transport->ReverseLookupAddress(*escaping_remote)
                        .Serialize()
                        .c_str(),
                    owned_buffer.size());
            }

            proto::Message message;
            PBMessage pb(message);
            SignedAdapter security(pb, "");
            Layout tom(security, true, replicaIdx);
            tom.Parse(owned_buffer.data(), owned_buffer.size());
            if (!tom.IsVerified() || !security.IsVerified()) {
                RWarning(
                    "Failed to verify TOM packet: tom = %d, request = %d",
                    tom.IsVerified(), security.IsVerified());
                return nullptr;
            }
            switch (message.get_case()) {
            case proto::Message::GetCase::kRequest:
                return [ //
                           this, escaping_remote, message, tom, owned_buffer] {
                    auto remote =
                        std::unique_ptr<TransportAddress>(escaping_remote);
                    query_buffer[tom.MessageNumber()] = owned_buffer;
                    HandleRequest(*remote, message.request(), tom);
                    ConcludeEpilogue();
                };

            case proto::Message::GetCase::kViewChange:
                return [this, escaping_remote, message] {
                    auto remote =
                        std::unique_ptr<TransportAddress>(escaping_remote);
                    HandleViewChange(*remote, message.view_change());
                    ConcludeEpilogue();
                };
            case proto::Message::GetCase::kViewStart:
                return [this, escaping_remote, message] {
                    auto remote =
                        std::unique_ptr<TransportAddress>(escaping_remote);
                    HandleViewStart(*remote, message.view_start());
                    ConcludeEpilogue();
                };
            case proto::Message::GetCase::kEpochStart:
                return [this, escaping_remote, message] {
                    auto remote =
                        std::unique_ptr<TransportAddress>(escaping_remote);
                    HandleEpochStart(*remote, message.epoch_start());
                    ConcludeEpilogue();
                };

            case proto::Message::GetCase::kQuery:
                // {
                //     auto remote =
                //         std::unique_ptr<TransportAddress>(escaping_remote);
                //     const auto &query = message.query();
                RDebug(
                    "Receiving Query: replica id = %d, message number = %u",
                    message.query().replica_index(),
                    message.query().message_number());
                //     if (!query_buffer.count(query.message_number())) {
                //         RWarning("Ignore unseen queried");
                //         return nullptr;
                //     }
                //     const auto &message =
                //     query_buffer[query.message_number()];
                //     transport->SendMessage(
                //         this, *remote,
                //         BufferMessage(message.data(), message.size()));
                //     return nullptr;
                // }
                return [this, escaping_remote, message] {
                    auto remote =
                        std::unique_ptr<TransportAddress>(escaping_remote);
                    HandleQueryMessage(*remote, message.query());
                    ConcludeEpilogue();
                };
                // case proto::Message::GetCase::kQueryReply: {
                //     if (status != STATUS_GAP_COMMIT) {
                //         return nullptr;
                //     }
                //     if ( //
                //         message.query_reply().message_number() !=
                //         queried_message_number) {
                //         return nullptr;
                //     }
                //     proto::Message reply;
                //     PBMessage pb_reply(reply);
                //     SignedAdapter signed_reply(pb_reply, "");
                //     Layout tom_reply(tom_reply, true, replicaIdx);
                //     tom_reply.Parse(
                //         message.query_reply().queried().data(),
                //         message.query_reply().queried().size());
                //     if (!tom_reply.IsVerified() ||
                //     !signed_reply.IsVerified()) {
                //         RWarning("Failed to verify QueryReply");
                //         return nullptr;
                //     }

                //     return [ //
                //                this, escaping_remote, reply, tom_reply] {
                //         auto remote =
                //             std::unique_ptr<TransportAddress>(escaping_remote);
                //         status = STATUS_NORMAL;
                //         HandleRequest(*remote, reply.request(), tom_reply);
                //         ResumeBuffered();
                //         ConcludeEpilogue();
                //     };
                // }

            default:
                RPanic("Unexpected message case: %d", message.get_case());
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
    //         auto remote = std::unique_ptr<TransportAddress>(escaping_remote);
    //         ReceiveMessage(*remote, &nic_message.front(),
    //         nic_message.size());
    //     });
    // }
}

class TOMBFTReplica : public TOMBFTReplicaCommon<TOMBFTAdapter> {
    void HandleRequest(
        const TransportAddress &remote, const Request &message,
        const TOMBFTAdapter &meta) override;

public:
    TOMBFTReplica(
        const Configuration &config, int replica_index,
        const string &identifier, int worker_thread_count, Transport *transport,
        AppReplica *app)
        : TOMBFTReplicaCommon<TOMBFTAdapter>(
              config, replica_index, identifier, worker_thread_count, transport,
              app) {
        high_verified = 0;
    }
};

class TOMBFTHMACReplica : public TOMBFTReplicaCommon<TOMBFTHMACAdapter> {
    void HandleRequest(
        const TransportAddress &remote, const Request &message,
        const TOMBFTHMACAdapter &meta) override;

public:
    TOMBFTHMACReplica(
        const Configuration &config, int replica_index,
        const string &identifier, int worker_thread_count, Transport *transport,
        AppReplica *app)
        : TOMBFTReplicaCommon<TOMBFTHMACAdapter>(
              config, replica_index, identifier, worker_thread_count, transport,
              app) {
        high_verified =
            UINT64_MAX; // all TOMBFT-HMAC messages are self verified
    }
};

#undef RPanic
#undef RDebug
#undef RNotice
#undef RWarning

} // namespace tombft
} // namespace dsnet