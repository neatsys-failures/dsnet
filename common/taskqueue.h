#pragma once
#include "lib/transport.h"
#include "lib/ctpl.h"

#include <functional>
#include <queue>
#include <future>
#include <memory>

namespace dsnet {

// General parallization for message-preprocessing.
// Compare to native worker thread pool:
// * PrologueQueue is passive. It relies on external calling for both enqueue/dequeue.
// * PrologueQueue is ordered. It will not release task that enqueue later first.
// * PrologueQueue is specific for message-processing.
class PrologueQueue {
public:
    using OwnedMessage = std::unique_ptr<Message>;
    // Prologue callback should:
    // * take ownership of message. It is callback's responsibility to keep message alive
    //   before returning it.
    // * be stateless. Any processing logic that require mutating replica state should be
    //   done by replica sequencially.
    // * optionally decide whether the message is dropped. If callback drops the message,
    //   it returns nullptr instead of the message.
    using Prologue = std::function<OwnedMessage (OwnedMessage)>;
    PrologueQueue(int nb_thread);
    void Enqueue(OwnedMessage message, Prologue prologue);
    OwnedMessage Dequeue();
private:
    std::queue<std::future<OwnedMessage>> tasks;
    ctpl::thread_pool pool;
};

}