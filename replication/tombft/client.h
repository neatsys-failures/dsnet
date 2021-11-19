#pragma once
#include "common/client.h"
#include "replication/tombft/message.pb.h"


namespace dsnet {
namespace tombft {

class TOMBFTClient : public Client
{
public:
    TOMBFTClient(
        const Configuration &config, 
        const ReplicaAddress &addr, const std::string identifier,
        Transport *transport, uint64_t clientid = 0);
    virtual ~TOMBFTClient();
    virtual void Invoke(const string &request, continuation_t continuation) override;
    virtual void InvokeUnlogged(
        int replicaIdx, const string &request, continuation_t continuation,
        timeout_continuation_t timeoutContinuation = nullptr,
        uint32_t timeout = DEFAULT_UNLOGGED_OP_TIMEOUT) override;
    virtual void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t size) override;

protected:
    struct PendingRequest
    {
        string request;
        uint64_t clientid;
        uint64_t clientreqid;
        continuation_t continuation;
        bool received[16];
        inline PendingRequest(string request, uint64_t clientreqid, continuation_t continuation)
            : request(request), clientreqid(clientreqid), continuation(continuation) 
        { 
            for (int i = 0; i < 16; i += 1) {
                received[i] = false;
            }
        }
    };
    PendingRequest *pendingRequest;
    PendingRequest *pendingUnloggedRequest;
    Timeout *requestTimeout;
    uint64_t lastReqId;

    void HandleReply(
        const TransportAddress &remote, const proto::ReplyMessage &msg);
    // void HandleUnloggedReply(
    //     const TransportAddress &remote, const proto::UnloggedReplyMessage &msg);
    void SendRequest();
    void ResendRequest();

    const std::string identifier;
};

}
}
