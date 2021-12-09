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
    SpinOrderedRunner runner;
    uint64_t batch_size;

    // single states
    view_t view_number;
    opnum_t op_number, commit_number;
    std::unique_ptr<Timeout> close_batch_timeout;

    // aggregated states
    struct ClientEntry {
        opnum_t request_number;
        proto::Reply reply;
        bool has_reply;
        std::unique_ptr<TransportAddress> remote;
    };
    std::unordered_map<uint64_t, ClientEntry> client_table;
    Log log;
    // op number -> digest -> replica id -> message
    std::unordered_map<
        opnum_t, //
        std::unordered_map<std::string, std::unordered_map<int, std::string>>>
        prepare_quorum, commit_quorum;
    std::map<opnum_t, Request> request_buffer;

    std::vector<std::string> request_batch; // List[Signed[Request]]

    bool IsPrimary() const {
        return configuration.GetLeaderIndex(view_number) == replicaIdx;
    }

    void HandleRequest(
        const TransportAddress &remote, const Request &request,
        const std::string &signed_message);
    void HandlePreprepare(
        const TransportAddress &remote, const proto::Prepare &prepare,
        const std::string &signed_prepare,
        const std::vector<Request> &requests);
    void HandlePrepare(
        const TransportAddress &remote, const proto::Prepare &prepare,
        const std::string &signed_prepare);
    void HandleCommit(
        const TransportAddress &remote, const proto::Commit &commit,
        const std::string &signed_commit);

    void InsertPrepare(
        const proto::Prepare &prepare, const std::string &signed_prepare);
    void CloseBatch();
};

} // namespace pbft
} // namespace dsnet
