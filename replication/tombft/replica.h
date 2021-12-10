#pragma once
#include "common/log.h"
#include "common/replica.h"
#include "common/runner.h"
#include "lib/transport.h"
#include "replication/tombft/adapter.h"
#include "replication/tombft/message.pb.h"

namespace dsnet {
namespace tombft {

class TOMBFTReplica : public Replica {
public:
    TOMBFTReplica(
        const Configuration &config, int replica_index,
        const string &identifier, int worker_thread_count, Transport *transport,
        AppReplica *app);

    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t size) override;

private:
    SpinOrderedRunner runner;
    const string identifier;

    uint32_t last_message_number;
    uint64_t last_executed;
    uint8_t session_number;

    Log log;
    std::unordered_map<uint64_t, proto::ReplyMessage> client_table;
    std::unordered_map<uint64_t, std::unique_ptr<TransportAddress>>
        address_table;

    std::map<uint32_t, Request> tom_buffer;

    void HandleRequest(
        TransportAddress &remote, Request &message, TOMBFTAdapter &meta);

    void ExecuteOne(Request &message); // insert into log by the way

    std::vector<Runner::Epilogue> epilogue_list;
    void ConcludeEpilogue() {
        runner.RunEpilogue([epilogue_list = this->epilogue_list] {
            for (Runner::Epilogue epilogue : epilogue_list) {
                epilogue();
            }
        });
        epilogue_list.clear();
    }
};

} // namespace tombft
} // namespace dsnet