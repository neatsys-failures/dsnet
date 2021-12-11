#include "replication/tombft/replica.h"

#include "common/pbmessage.h"
#include "common/signedadapter.h"
#include "lib/message.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, this->replicaIdx, ##__VA_ARGS__)

namespace dsnet {
namespace tombft {

using std::move;
using std::string;
using std::unique_ptr;
using PBAdapter = PBMessage;

static int n_message = 0, n_signed_message = 0;

TOMBFTReplica::~TOMBFTReplica() {
    RNotice(
        "n_message = %d, n_signed_message = %d", n_message, n_signed_message);
    RNotice(
        "average batch size = %d", (n_message + 1) / (n_signed_message + 1));
}

void TOMBFTReplica::HandleRequest(
    TransportAddress &remote, Request &message, TOMBFTAdapter &meta //
) {
    if (!address_table[message.clientid()]) {
        address_table[message.clientid()] =
            unique_ptr<TransportAddress>(remote.clone());
    }

    // convinent hack
    if (session_number == 0) {
        session_number = meta.SessionNumber();
    }
    if (start_number > meta.MessageNumber()) {
        start_number = meta.MessageNumber();
    }
    // RNotice(
    //     "message number = %u, is signed = %d", meta.MessageNumber(),
    //     meta.IsSigned());

    if (session_number != meta.SessionNumber()) {
        RWarning(
            "Session changed: %u -> %u", session_number, meta.SessionNumber());
        NOT_IMPLEMENTED();
    }

    if (meta.MessageNumber() < start_number + last_executed) {
        return; // either it is signed or not, it is useless
    }

    n_message += 1;
    // TODO save all message (with duplicated message number) for BFT
    // RNotice("buffering %d", meta.MessageNumber());
    tom_buffer[meta.MessageNumber()] = message;

    if (meta.IsSigned()) {
        n_signed_message += 1;
        auto iter = tom_buffer.begin();
        while (iter != tom_buffer.end()) {
            if (iter->first > meta.MessageNumber()) {
                break;
            }
            // RNotice("executing %d", iter->first);
            if (iter->first != start_number + last_executed) {
                RPanic(
                    "Gap: %lu (+%lu)", start_number + last_executed,
                    iter->first - (start_number + last_executed));
                NOT_IMPLEMENTED(); // state transfer
            }
            ExecuteOne(iter->second);
            iter = tom_buffer.erase(iter);
        }
        return;
    }
}

void TOMBFTHMACReplica::HandleRequest(
    TransportAddress &remote, Request &message, TOMBFTHMACAdapter &meta //
) {
    if (!address_table[message.clientid()]) {
        address_table[message.clientid()] =
            unique_ptr<TransportAddress>(remote.clone());
    }

    // convinent hack
    if (session_number == 0) {
        session_number = meta.SessionNumber();
    }
    if (start_number > meta.MessageNumber()) {
        start_number = meta.MessageNumber();
    }
    // RNotice(
    //     "message number = %u, is signed = %d", meta.MessageNumber(),
    //     meta.IsSigned());

    if (session_number != meta.SessionNumber()) {
        RWarning(
            "Session changed: %u -> %u", session_number, meta.SessionNumber());
        NOT_IMPLEMENTED();
    }

    if (meta.MessageNumber() < start_number + last_executed) {
        return; // either it is signed or not, it is useless
    }

    n_message += 1;
    if (meta.MessageNumber() != start_number + last_executed) {
        NOT_IMPLEMENTED(); // state transfer
    }

    ExecuteOne(message);
}

} // namespace tombft
} // namespace dsnet