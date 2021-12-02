#pragma once
#include "common/client.h"
#include "replication/pbft/message.pb.h"

namespace dsnet {
namespace pbft {

class PBFTClient : public Client {
public:
    PBFTClient(
        const Configuration &config, const ReplicaAddress &addr,
        const std::string &identifier, Transport *transport);
    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t len) override;
    void Invoke(const std::string &op, continuation_t then) override;
    void InvokeUnlogged(
        int replica_id, const std::string &op, continuation_t then,
        timeout_continuation_t timeout_then, uint32_t timeout) override;

private:
    std::string identifier;

    view_t view_number;
    opnum_t request_number;
    struct PendingRequest {
        opnum_t request_number;
        proto::PBFTMessage message;
        continuation_t then;
        bool received[16];
    };
    std::unique_ptr<PendingRequest> pending_request;

    std::unique_ptr<Timeout> resend_timeout;

    void SendRequest(bool broadcast);
};

} // namespace pbft
} // namespace dsnet