#include "replication/tombft/replica.h"

#include "common/pbmessage.h"

namespace dsnet {
namespace tombft {

using std::string;
using std::unique_ptr;
using std::move;
using PBAdapter = PBMessage;

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
    runner.RunPrologue([
        this,
        escaping_remote = remote.clone(),
        owned_buffer = string((const char *) buf, size)
    ]() -> Runner::Solo {
        auto message = unique_ptr<proto::Message>(new proto::Message);
        PBAdapter pb(*message);
        auto tom = unique_ptr<TOMBFTAdapter>(new TOMBFTAdapter(pb, "", false));
        tom->Parse(owned_buffer.data(), owned_buffer.size());
        if (!tom->IsVerified()) {
            //
            return nullptr;
        }
        switch (message->get_case()) {
        case proto::Message::GetCase::kRequest:
            return [
                this,
                escaping_remote,
                escaping_message = message.release(),
                escaping_tom = tom.release()
            ]() {
                auto remote = unique_ptr<TransportAddress>(escaping_remote);
                auto message = unique_ptr<proto::Message>(escaping_message);
                auto tom = unique_ptr<TOMBFTAdapter>(escaping_tom);
                HandleRequest(*remote, *message->mutable_request(), *tom);
            };
        default:
            Panic("Unexpected message case: %d", message->get_case());
        }

        return nullptr;
    });
}

void TOMBFTReplica::HandleRequest(
    TransportAddress &remote, Request &message, TOMBFTAdapter &meta)
{
    //
}


}
}