#pragma once
#include "common/client.h"
#include "lib/assert.h"
#include "replication/hotstuff/message.pb.h"

namespace dsnet {
namespace hotstuff {

class HotStuffClient : public Client {
public:
    HotStuffClient(
        const Configuration &config, const ReplicaAddress &addr,
        const std::string &identifier, Transport *transport);
    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t len) override;
    void Invoke(const std::string &op, continuation_t then) override;
    void InvokeUnlogged(
        int replica_id, const std::string &op, continuation_t then,
        timeout_continuation_t timeout_then,
        uint32_t timeout = DEFAULT_UNLOGGED_OP_TIMEOUT //
        ) override {
        NOT_IMPLEMENTED();
    }

private:
    string identifier;

    std::unique_ptr<Timeout> resend_request_timeout;
    opnum_t request_number;
    struct PendingRequest {
        opnum_t request_number;
        std::string op;
        proto::Message message;
        bool received[16];
        continuation_t then;
        PendingRequest(
            opnum_t request_number, const std::string &op,
            proto::Message message, continuation_t then)
            : request_number(request_number), op(op), message(message),
              then(then) //
        {
            for (int i = 0; i < 16; i += 1) {
                received[i] = false;
            }
        }
    };
    std::unique_ptr<PendingRequest> pending_request;

    void HandleReply(
        const TransportAddress &remote, const proto::ReplyMessage &reply);
    void SendRequest();
};

} // namespace hotstuff
} // namespace dsnet
