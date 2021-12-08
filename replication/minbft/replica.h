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
    // since MinBFTAdapter layer is not stored here, upon message handling the
    // verification info is lost
    // however the spec seems not require receiver side to recover any cert,
    // it just requires sender side to log everything sent
    // we made some shit design
    struct PendingUIMessage {
        proto::UIMessage ui_message;
        Request request;
    };
    std::unordered_map<int, std::map<uint64_t, PendingUIMessage>> ui_queue;
    std::unordered_map<int, opnum_t> next_ui;

    QuorumSet<opnum_t, proto::UIMessage> commit_quorum;

    void HandleUIMessage(
        const TransportAddress &remote, const proto::UIMessage &ui_message,
        opnum_t ui, const Request &request);
    void HandleUIMessageInternal(
        const TransportAddress &remote, const proto::UIMessage &ui_message,
        opnum_t ui, const Request &request);

    view_t view_number;
};

} // namespace minbft
} // namespace dsnet
