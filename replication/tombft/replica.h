#pragma once
#include "lib/transport.h"
#include "common/replica.h"
#include "common/runner.h"
#include "replication/tombft/message.pb.h"
#include "replication/tombft/adapter.h"

namespace dsnet {
namespace tombft {

class TOMBFTReplica: public Replica {
public:
    TOMBFTReplica(
        const Configuration &config, int replica_index, const string &identifier,
        int worker_thread_count, Transport *transport, AppReplica *app);

    void ReceiveMessage(const TransportAddress &remote, void *buf, size_t size) override;

private:
    Runner runner;
    string identifier;

    void HandleRequest(
        TransportAddress &remote, Request &message, TOMBFTAdapter &meta);
};


}
}