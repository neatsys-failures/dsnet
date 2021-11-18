#include "replication/tombft/replica.h"

#include "replication/tombft/adapter.h"

namespace dsnet {
namespace tombft {

using std::string;

TOMBFTReplica::TOMBFTReplica(
    const Configuration &config, int replica_index, const string &identifier,
    int worker_thread_count, Transport *transport, AppReplica *app)
    : Replica(config, 0, replica_index, true, transport, app),
    runner(worker_thread_count), identifier(identifier)
{
    //
}

void TOMBFTReplica::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size) 
{
    //
}

}
}