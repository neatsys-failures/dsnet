#pragma once
#include "common/quorumset.h"
#include "common/replica.h"
#include "common/request.pb.h"
#include "common/runner.h"
#include "replication/minbft/message.pb.h"

namespace dsnet {
namespace minbft {

class MinBFTReplica : public Replica {
public:
    MinBFTReplica(
        const Configuration &config, int replica_id,
        const std::string &identifier, int n_worker, int batch_size,
        Transport *transport, AppReplica *app);
    ~MinBFTReplica();

    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t len) override;

private:
    const std::string identifier;
    CTPLOrderedRunner runner;

    // according to spec, all UI message (i.e. message that takes a UI) must be
    // handled in FIFO order
    // replica id -> UI -> message
    std::unordered_map<int, std::map<uint64_t, Runner::Solo>> ui_queue;
    std::unordered_map<int, opnum_t> next_ui;

    QuorumSet<opnum_t, proto::Commit> commit_quorum;

    view_t view_number;
    opnum_t commit_number;
    Log log;
    struct ClientEntry {
        std::unique_ptr<TransportAddress> remote;
        uint64_t request_number;
        bool ready;
        proto::Reply reply;
    };
    std::unordered_map<uint64_t, ClientEntry> client_table;

    void HandlePrepare(
        const TransportAddress &remote, const proto::Prepare &prepare,
        opnum_t ui, const Request &request);
    void
    HandleCommit(const TransportAddress &remote, const proto::Commit &commit);

    void AddCommit(const proto::Commit &commit);
};

} // namespace minbft
} // namespace dsnet
