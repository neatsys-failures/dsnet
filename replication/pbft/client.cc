#include "replication/pbft/client.h"
#include "common/pbmessage.h"
#include "common/signedadapter.h"
#include "lib/assert.h"

namespace dsnet {
namespace pbft {

using std::string;
using std::unique_ptr;

PBFTClient::PBFTClient(
    const Configuration &config, const ReplicaAddress &addr,
    const string &identifier, Transport *transport)
    : Client(config, addr, transport), identifier(identifier) //
{
    request_number = 0;
    view_number = 0;
    pending_request = nullptr;
    resend_timeout = unique_ptr<Timeout>(new Timeout(transport, 1000, [this]() {
        Warning(
            "Resend: request number = %lu", pending_request->request_number);
        SendRequest(true);
    }));
}

void PBFTClient::InvokeUnlogged(
    int replica_id, const string &op, continuation_t then,
    timeout_continuation_t timeout_then, uint32_t timeout) {
    NOT_IMPLEMENTED();
}

void PBFTClient::Invoke(const string &op, continuation_t then) {
    if (pending_request != nullptr) {
        Panic("Duplicated pending request");
    }
    request_number += 1;
    pending_request = unique_ptr<PendingRequest>(new PendingRequest);
    pending_request->request_number = request_number;
    pending_request->then = then;
    for (int i = 0; i < config.n; i += 1) {
        pending_request->received[i] = false;
    }

    proto::PBFTMessage message;
    auto &request = *message.mutable_request();
    request.set_clientid(clientid);
    request.set_clientreqid(request_number);
    request.set_op(op);
    // request.set_clientaddr(node_addr_->Serialize());
    pending_request->message = message;

    // to let replicas logging client address
    SendRequest(request_number == 1);
}

void PBFTClient::SendRequest(bool broadcast) {
    resend_timeout->Reset();
    PBMessage pb_layer(pending_request->message);
    SignedAdapter signed_layer(pb_layer, identifier, false);
    if (!broadcast) {
        transport->SendMessageToReplica(
            this, config.GetLeaderIndex(view_number), signed_layer);
    } else {
        transport->SendMessageToAll(this, signed_layer);
    }
}

void PBFTClient::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t len //
) {
    if (pending_request == nullptr) {
        return;
    }

    proto::Reply reply;
    PBMessage pb_layer(reply);
    pb_layer.Parse(buf, len);
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

    view_number = reply.view_number();
    auto then = pending_request->then;
    auto op = pending_request->message.request().op();
    pending_request.reset();
    then(op, reply.result());
}

} // namespace pbft
} // namespace dsnet