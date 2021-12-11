#include "replication/tombft/client.h"

#include "common/pbmessage.h"
#include "common/signedadapter.h"
#include "lib/assert.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "replication/tombft/adapter.h"

namespace dsnet {
namespace tombft {

using namespace proto;
using std::string;

TOMBFTClient::TOMBFTClient(
    const Configuration &config, const ReplicaAddress &addr,
    const string identifier, bool use_hmac, Transport *transport,
    uint64_t clientid)
    : Client(config, addr, transport, clientid), identifier(identifier),
      use_hmac(use_hmac) {
    pendingRequest = NULL;
    pendingUnloggedRequest = NULL;
    lastReqId = 0;
    requestTimeout =
        new Timeout(transport, 1000, [this]() { ResendRequest(); });
}

TOMBFTClient::~TOMBFTClient() {
    if (pendingRequest) {
        delete pendingRequest;
    }
    if (pendingUnloggedRequest) {
        delete pendingUnloggedRequest;
    }
}

void TOMBFTClient::Invoke(const string &request, continuation_t continuation) {
    // XXX Can only handle one pending request for now
    if (pendingRequest != NULL) {
        Panic("Client only supports one pending request");
    }

    lastReqId += 1;
    pendingRequest = new PendingRequest(request, lastReqId, continuation);

    SendRequest();
}

void TOMBFTClient::SendRequest() {
    proto::Message m;
    Request *reqMsg = m.mutable_request();
    reqMsg->set_op(pendingRequest->request);
    reqMsg->set_clientid(clientid);
    reqMsg->set_clientreqid(lastReqId);
    // reqMsg->set_clientaddr(node_addr_->Serialize());

    PBMessage pb_m(m);
    SignedAdapter signed_layer(pb_m, identifier);
    if (use_hmac) {
        transport->SendMessageToMulticast(
            this, TOMBFTHMACAdapter(signed_layer, -1, true));
    } else {
        transport->SendMessageToMulticast(
            this, TOMBFTAdapter(signed_layer, true));
    }

    requestTimeout->Reset();
}

void TOMBFTClient::ResendRequest() {
    // NOT_REACHABLE();
    Warning("Timeout, resending request for req id %lu", lastReqId);
    SendRequest();
}

void TOMBFTClient::InvokeUnlogged(
    int replicaIdx, const string &request, continuation_t continuation,
    timeout_continuation_t timeoutContinuation, uint32_t timeout) {
    NOT_IMPLEMENTED();
}

void TOMBFTClient::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size) {
    static proto::Message client_msg;
    static PBMessage m(client_msg);
    static SignedAdapter signed_adapter(m, "");

    signed_adapter.Parse(buf, size);
    ASSERT(signed_adapter.IsVerified());

    switch (client_msg.get_case()) {
    case proto::Message::GetCase::kReply:
        HandleReply(remote, client_msg.reply());
        break;
    // case ToClientMessage::MsgCase::kUnloggedReply:
    //     HandleUnloggedReply(remote, client_msg.unlogged_reply());
    //     break;
    default:
        Panic("Received unexpected message type: %u", client_msg.get_case());
    }
}

void TOMBFTClient::HandleReply(
    const TransportAddress &remote, const proto::ReplyMessage &msg) {
    if (pendingRequest == NULL) {
        // Warning("Received reply when no request was pending");
        return;
    }

    if (msg.client_request() != pendingRequest->clientreqid) {
        return;
    }

    pendingRequest->received[msg.replica_index()] = true;
    int count = 0;
    for (int i = 0; i < config.n; i += 1) {
        if (pendingRequest->received[i]) {
            count += 1;
        }
    }
    Debug("Packet count = %d", count);
    if (count < 2 * config.f + 1) {
        // if (count < config.n) {
        return;
    }

    Debug("Client received reply");

    requestTimeout->Stop();

    PendingRequest *req = pendingRequest;
    pendingRequest = NULL;

    req->continuation(req->request, msg.result());
    delete req;
}

// void
// SignedUnrepClient::HandleUnloggedReply(
//     const TransportAddress &remote, const proto::UnloggedReplyMessage &msg)
// {
//     NOT_REACHABLE();
// }

} // namespace tombft
} // namespace dsnet
