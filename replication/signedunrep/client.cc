// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * unreplicated/client.cc:
 *   dummy unreplicated client
 *
 * Copyright 2013 Dan R. K. Ports  <drkp@cs.washington.edu>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/

#include "common/client.h"
#include "common/request.pb.h"
#include "common/pbmessage.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "replication/signedunrep/client.h"
#include "replication/signedunrep/signedunrep-proto.pb.h"

namespace dsnet {
namespace signedunrep {

using namespace proto;

SignedUnrepClient::SignedUnrepClient(const Configuration &config,
                                       const ReplicaAddress &addr,
                                       Transport *transport,
                                       uint64_t clientid)
    : Client(config, addr, transport, clientid)
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
SignedUnrepClient::Invoke(const string &request,
                           continuation_t continuation)
{
    // XXX Can only handle one pending request for now
    if (pendingRequest != NULL) {
        Panic("Client only supports one pending request");
    }

    ++lastReqId;
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

    // SignedUnrep: just send to replica 0
    transport->SendMessageToReplica(this, 0, PBMessage(m));

    requestTimeout->Reset();
}

void
SignedUnrepClient::ResendRequest()
{
    Warning("Timeout, resending request for req id %lu", lastReqId);
    SendRequest();
}

void
SignedUnrepClient::InvokeUnlogged(int replicaIdx,
                                   const string &request,
                                   continuation_t continuation,
                                   timeout_continuation_t timeoutContinuation,
                                   uint32_t timeout)
{
    // XXX Can only handle one pending request for now
    if (pendingUnloggedRequest != NULL) {
        Panic("Client only supports one pending request");
    }

    pendingUnloggedRequest = new PendingRequest(request, 0, continuation);

    ToReplicaMessage m;
    UnloggedRequestMessage *reqMsg = m.mutable_unlogged_request();
    reqMsg->mutable_req()->set_op(pendingUnloggedRequest->request);
    reqMsg->mutable_req()->set_clientid(clientid);
    reqMsg->mutable_req()->set_clientreqid(0);

    // SignedUnrep: just send to replica 0
    if (replicaIdx != 0) {
        Panic("Attempt to invoke unlogged operation on replica that doesn't exist");
    }
    transport->SendMessageToReplica(this, 0, PBMessage(m));

}

void
SignedUnrepClient::ReceiveMessage(const TransportAddress &remote,
                                   void *buf, size_t size)
{
    static ToClientMessage client_msg;
    static PBMessage m(client_msg);

    m.Parse(buf, size);

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
SignedUnrepClient::HandleReply(const TransportAddress &remote,
                                const proto::ReplyMessage &msg)
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
SignedUnrepClient::HandleUnloggedReply(const TransportAddress &remote,
                                const proto::UnloggedReplyMessage &msg)
{
    if (pendingUnloggedRequest == NULL) {
        Warning("Received unloggedReply when no request was pending");
    }

    Debug("Client received unloggedReply");

    PendingRequest *req = pendingUnloggedRequest;
    pendingUnloggedRequest = NULL;

    req->continuation(req->request, msg.reply());
    delete req;
}

} 
} 
