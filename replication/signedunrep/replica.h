#pragma once

#include "common/replica.h"
#include "common/runner.h"
#include "replication/signedunrep/signedunrep-proto.pb.h"

#include "common/log.h"

namespace dsnet {
namespace signedunrep {

class SignedUnrepReplica : public Replica {
public:
    SignedUnrepReplica(
        Configuration config, std::string identifier, int n_worker,
        int batch_size, Transport *transport, AppReplica *app);
    ~SignedUnrepReplica();
    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t size) override;

private:
    void HandleRequest(
        const TransportAddress &remote, const proto::RequestMessage &msg);
    void HandleUnloggedRequest(
        const TransportAddress &remote,
        const proto::UnloggedRequestMessage &msg);

    void
    UpdateClientTable(const Request &req, const proto::ToClientMessage &reply);

    opnum_t last_op;
    Log log;
    struct ClientTableEntry {
        uint64_t lastReqId;
        proto::ToClientMessage reply;
        std::unique_ptr<TransportAddress> remote;
    };
    std::map<uint64_t, ClientTableEntry> clientTable;

    const std::string identifier;
    SpinOrderedRunner runner;
    size_t batch_size;
    std::vector<Request> request_batch;

    std::unique_ptr<Timeout> close_batch_timeout;
    void CloseBatch();
};

} // namespace signedunrep
} // namespace dsnet
