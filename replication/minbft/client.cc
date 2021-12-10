#include "replication/minbft/client.h"

#include "common/pbmessage.h"
#include "common/signedadapter.h"

namespace dsnet {
namespace minbft {

using std::string;
using std::unique_ptr;

MinBFTClient::MinBFTClient(
    const Configuration &config, const ReplicaAddress &addr,
    const string &identifier, Transport *transport)
    : Client(config, addr, transport), identifier(identifier) //
{
    resend_request_timeout =
        unique_ptr<Timeout>(new Timeout(transport, 1000, [this]() {
            // NOT_REACHABLE();  // debug
            Warning(
                "Resend request: request number = %lu",
                pending_request->request_number);
            SendRequest();
        }));

    request_number = 0;
}

void MinBFTClient::Invoke(const std::string &op, continuation_t then) {
    if (pending_request) {
        Panic("Duplicated pending request");
    }
    request_number += 1;
    Request request;
    request.set_clientid(clientid);
    request.set_clientreqid(request_number);
    request.set_op(op);
    PBMessage pb_request(request);
    SignedAdapter signed_request(pb_request, identifier);
    string buffer;
    buffer.resize(signed_request.SerializedSize());
    signed_request.Serialize(&buffer.front());
    proto::MinBFTMessage message;
    *message.mutable_signed_request() = buffer;
    pending_request = unique_ptr<PendingRequest>(
        new PendingRequest(request_number, op, message, then));
    SendRequest();
}

void MinBFTClient::SendRequest() {
    transport->SendMessageToAll(this, PBMessage(pending_request->message));
    resend_request_timeout->Reset();
}

void MinBFTClient::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t len //
) {
    if (!pending_request) {
        return;
    }
    proto::Reply reply;
    PBMessage pb_layer(reply);
    SignedAdapter signed_layer(pb_layer, "");
    signed_layer.Parse(buf, len);
    if (reply.request_number() != pending_request->request_number) {
        return;
    }
    pending_request->received[reply.replica_id()] = true;

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

} // namespace minbft
} // namespace dsnet