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

template <typename Layout> struct LayoutToRunner {};
template <> struct LayoutToRunner<TOMBFTAdapter> { using Runner = CTPLRunner; };
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
            core_id = 3;
            break;
        case 2:
            core_id = 6;
            break;
        case 3:
            core_id = 9;
            break;
        case 4:
            core_id = 12;
            break;
        case 5:
            core_id = 32;
            break;
        case 6:
            core_id = 35;
            break;
        case 7:
            core_id = 38;
            break;
        case 8:
            core_id = 41;
            break;
        case 9:
            core_id = 44;
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

    uint32_t start_number;
    uint64_t last_executed;
    uint8_t session_number;

    Log log;
    std::unordered_map<uint64_t, proto::ReplyMessage> client_table;
    std::unordered_map<uint64_t, std::unique_ptr<TransportAddress>>
        address_table;

    std::map<uint32_t, Request> tom_buffer;
    void ExecuteOne(Request &message); // insert into log by the way

    std::vector<Runner::Epilogue> epilogue_list;
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

    virtual void
    HandleRequest(TransportAddress &remote, Request &message, Layout &meta) = 0;

    void ConcludeEpilogue() {
        if (epilogue_list.empty()) {
            return;
        }
        runner.RunEpilogue([runner_list = epilogue_list] {
            for (Runner::Epilogue epilogue : runner_list) {
                epilogue();
            }
        });
        epilogue_list.clear();
    }
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