#pragma once
#include "common/replica.h"
#include "replication/pbft/message.pb.h"

namespace dsnet {
namespace pbft {

class PBFTReplica : public Replica {
public:
    PBFTReplica(
        const Configuration &config, int replica_id, std::string identifier,
        int n_worker_thread);
    ~PBFTReplica();

    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t len) override;

private:
    //
};

} // namespace pbft
} // namespace dsnet
