#include "replication/signedunrep/client.h"

#include "lib/message.h"
#include "lib/assert.h"
#include "lib/transport.h"
#include "common/client.h"
#include "common/pbmessage.h"
#include "common/signedadapter.h"
#include "replication/signedunrep/signedunrep-proto.pb.h"

namespace dsnet {
namespace signedunrep {

using namespace proto;
using std::string;

SignedUnrepClient::SignedUnrepClient(
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

SignedUnrepClient::~SignedUnrepClient()
{
    if (pendingRequest) {
        delete pendingRequest;
    }
    if (pendingUnloggedRequest) {
        delete pendingUnloggedRequest;
    }
}

void
SignedUnrepClient::Invoke(const string &request, continuation_t continuation)
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
SignedUnrepClient::SendRequest()
{
    ToReplicaMessage m;
    RequestMessage *reqMsg = m.mutable_request();
    reqMsg->mutable_req()->set_op(pendingRequest->request);
    reqMsg->mutable_req()->set_clientid(clientid);
    reqMsg->mutable_req()->set_clientreqid(lastReqId);

    PBMessage pb_m(m);
    transport->SendMessageToReplica(this, 0, SignedAdapter(pb_m, identifier));

    requestTimeout->Reset();
}

void
SignedUnrepClient::ResendRequest()
{
    Warning("Timeout, resending request for req id %lu", lastReqId);
    SendRequest();
}

void
SignedUnrepClient::InvokeUnlogged(
    int replicaIdx, const string &request,
    continuation_t continuation, timeout_continuation_t timeoutContinuation,
    uint32_t timeout)
{
    NOT_IMPLEMENTED();
}

void
SignedUnrepClient::ReceiveMessage(
    const TransportAddress &remote, void *buf, size_t size)
{
    static ToClientMessage client_msg;
    static PBMessage m(client_msg);
    static SignedAdapter signed_adapter(m, "Steve");  // TODO: get remote id from configure

    signed_adapter.Parse(buf, size);
    ASSERT(signed_adapter.IsVerified());

    switch (client_msg.msg_case()) {
        case ToClientMessage::MsgCase::kReply:
            HandleReply(remote, client_msg.reply());
            break;
        case ToClientMessage::MsgCase::kUnloggedReply:
            HandleUnloggedReply(remote, client_msg.unlogged_reply());
            break;
        default:
            Panic("Received unexpected message type: %u",
                    client_msg.msg_case());
    }
}

void
SignedUnrepClient::HandleReply(
    const TransportAddress &remote, const proto::ReplyMessage &msg)
{
    if (pendingRequest == NULL) {
        Warning("Received reply when no request was pending");
	return;
    }

    if (msg.req().clientreqid() != pendingRequest->clientreqid) {
	return;
    }

    Debug("Client received reply");

    requestTimeout->Stop();

    PendingRequest *req = pendingRequest;
    pendingRequest = NULL;

    req->continuation(req->request, msg.reply());
    delete req;
}

void
SignedUnrepClient::HandleUnloggedReply(
    const TransportAddress &remote, const proto::UnloggedReplyMessage &msg)
{
    NOT_REACHABLE();
}

} 
} 
