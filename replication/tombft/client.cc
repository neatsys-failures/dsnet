#include "replication/tombft/client.h"

#include "lib/message.h"
#include "lib/assert.h"
#include "lib/transport.h"
#include "common/pbmessage.h"
#include "common/signedadapter.h"

namespace dsnet {
namespace tombft {

using namespace proto;
using std::string;

TOMBFTClient::TOMBFTClient(
    const Configuration &config, 
    const ReplicaAddress &addr, const string identifier,
    Transport *transport, uint64_t clientid)
    : Client(config, addr, transport, clientid), identifier(identifier)
{
    pendingRequest = NULL;
    pendingUnloggedRequest = NULL;
    lastReqId = 0;
    requestTimeout = new Timeout(transport, 1000, [this]() {
	    ResendRequest();
	});
}

TOMBFTClient::~TOMBFTClient()
{
    if (pendingRequest) {
        delete pendingRequest;
    }
    if (pendingUnloggedRequest) {
        delete pendingUnloggedRequest;
    }
}

void
TOMBFTClient::Invoke(const string &request, continuation_t continuation)
{
    // XXX Can only handle one pending request for now
    if (pendingRequest != NULL) {
        Panic("Client only supports one pending request");
    }

    lastReqId += 1;
    pendingRequest = new PendingRequest(request, lastReqId, continuation);

    SendRequest();
}

void
TOMBFTClient::SendRequest()
{
    proto::Message m;
    Request *reqMsg = m.mutable_request();
    reqMsg->set_op(pendingRequest->request);
    reqMsg->set_clientid(clientid);
    reqMsg->set_clientreqid(lastReqId);
    reqMsg->set_clientaddr(node_addr_->Serialize());

    PBMessage pb_m(m);
    transport->SendMessageToAll(this, SignedAdapter(pb_m, identifier));

    requestTimeout->Reset();
}

void
TOMBFTClient::ResendRequest()
{
    Warning("Timeout, resending request for req id %lu", lastReqId);
    SendRequest();
}

void
TOMBFTClient::InvokeUnlogged(
    int replicaIdx, const string &request,
    continuation_t continuation, timeout_continuation_t timeoutContinuation,
    uint32_t timeout)
{
    NOT_IMPLEMENTED();
}

void
TOMBFTClient::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size)
{
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

void
TOMBFTClient::HandleReply(
    const TransportAddress &remote, const proto::ReplyMessage &msg)
{
    if (pendingRequest == NULL) {
        Warning("Received reply when no request was pending");
    	return;
    }

    if (msg.client_request() != pendingRequest->clientreqid) {
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

} 
} 
