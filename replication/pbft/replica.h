#pragma once
#include "common/replica.h"
#include "common/runner.h"
#include "replication/pbft/message.pb.h"

namespace dsnet {
namespace pbft {

class PBFTReplica : public Replica {
public:
    PBFTReplica(
        const Configuration &config, int replica_id,
        const std::string &identifier, int n_worker, int batch_size,
        Transport *transport, AppReplica *app);
    ~PBFTReplica();

    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t len) override;

private:
    // consts
    string identifier;
    Runner runner;
    int batch_size;

    // single states
    view_t view_number;
    opnum_t op_number, commit_number;

    // aggregated states
    struct ClientEntry {
        std::unique_ptr<TransportAddress> remote;
        opnum_t request_number;
        proto::Reply reply;
        bool has_reply;
    };
    std::unordered_map<uint64_t, ClientEntry> client_table;
    Log log;

    bool IsPrimary() const {
        return configuration.GetLeaderIndex(view_number) == replicaIdx;
    }

    void HandleRequest(
        const TransportAddress &remote, const Request &request,
        const std::string &signed_message, const std::string &digest);
    void HandlePreprepare(
        const TransportAddress &remote, const proto::Preprepare &preprepare,
        const Request &request);
};

} // namespace pbft
} // namespace dsnet
