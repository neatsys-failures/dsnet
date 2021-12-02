#pragma once
#include "lib/transport.h"
#include "common/replica.h"
#include "common/runner.h"
#include "common/log.h"
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
    CTPLRunner runner;
    const string identifier;

    uint32_t last_message_number;
    uint64_t last_executed;
    uint8_t session_number;

    Log log;
    std::unordered_map<uint64_t, proto::ReplyMessage> client_table;

    void HandleRequest(
        TransportAddress &remote, Request &message, TOMBFTAdapter &meta);

    void ExecuteOne(Request &message);
};


}
}