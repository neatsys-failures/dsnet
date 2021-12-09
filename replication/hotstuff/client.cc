#include "replication/hotstuff/client.h"

#include "common/pbmessage.h"
#include "common/signedadapter.h"

namespace dsnet {
namespace hotstuff {

using std::string;
using std::unique_ptr;

HotStuffClient::HotStuffClient(
    const Configuration &config, const ReplicaAddress &addr,
    const string &identifier, Transport *transport)
    : Client(config, addr, transport), identifier(identifier) //
{
    resend_request_timeout =
        unique_ptr<Timeout>(new Timeout(transport, 1000, [this]() {
            Warning(
                "Resend request: request number = %lu",
                pending_request->request_number);
            SendRequest();
        }));

    request_number = 0;
}

void HotStuffClient::Invoke(const std::string &op, continuation_t then) {
    if (pending_request) {
        Panic("Duplicated pending request");
    }
    request_number += 1;
    proto::Message message;
    auto &request = *message.mutable_request();
    request.set_clientid(clientid);
    request.set_clientreqid(request_number);
    request.set_op(op);
    pending_request = unique_ptr<PendingRequest>(
        new PendingRequest(request_number, op, message, then));
    SendRequest();
}

void HotStuffClient::SendRequest() {
    PBMessage pb_layer(pending_request->message);
    SignedAdapter signed_layer(pb_layer, identifier, false);
    transport->SendMessageToAll(this, signed_layer);
    resend_request_timeout->Reset();
}

void HotStuffClient::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t len //
) {
    if (!pending_request) {
        return;
    }
    proto::ReplyMessage reply;
    PBMessage pb_layer(reply);
    pb_layer.Parse(buf, len);
    if (reply.client_request() != pending_request->request_number) {
        return;
    }
    pending_request->received[reply.replica_index()] = true;

    int count = 0;
    for (int i = 0; i < config.n; i += 1) {
        if (pending_request->received[i]) {
            count += 1;
        }
    }
    if (count < config.f + 1) {
        return;
    }
    // Notice(
    //     "Request done: client id = %lu, number = %lu", clientid,
    //     pending_request->request_number);
    resend_request_timeout->Stop();

    auto then = pending_request->then;
    auto op = pending_request->op;
    pending_request.reset();
    then(op, reply.result());
}

} // namespace hotstuff
} // namespace dsnet