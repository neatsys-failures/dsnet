#pragma once
#include "lib/transport.h"
#include "lib/ctpl.h"
#include "lib/assert.h"

#include <functional>
#include <queue>
#include <future>
#include <memory>
#include <cstring>

namespace dsnet {

template<typename M> class PrologueTask;
class AbstractPrologueTask {
public:
    virtual bool HasMessage() const = 0;
    virtual const TransportAddress &Remote() const = 0;
    virtual void ParseWithAdapter(class Message &adapter) const = 0;
    virtual ~AbstractPrologueTask() {
    }
    template<typename M> void SetMessage(M *message) {
        dynamic_cast<PrologueTask<M> &>(*this).message = message;
    }
    template<typename M> M &Message() {
        ASSERT(HasMessage());
        return *dynamic_cast<PrologueTask<M> &>(*this).message;
    }
};

template<typename M> class PrologueTask : public AbstractPrologueTask {
    friend class AbstractPrologueTask;
    bool HasMessage() const override {
        return message != nullptr;
    }
public:
    PrologueTask(void *buf, size_t len, const TransportAddress &remote_addr)
        : buf(new unsigned char[len]), len(len), remote(remote_addr.clone()), message(nullptr)
    {
        std::memcpy(this->buf, buf, len);
    }
    ~PrologueTask() {
        delete[] buf;
        delete remote;
        if (message != nullptr) {
            delete message;
        }
    }

    const TransportAddress &Remote() const override {
        return *remote;
    }
    void ParseWithAdapter(class Message &adapter) const override {
        adapter.Parse(buf, len);
    }
private:
    unsigned char *buf;
    size_t len;
    const TransportAddress *remote;
    M *message;
};

// General parallization for message-preprocessing.
// Compare to native worker thread pool:
// * PrologueQueue is passive. It relies on external calling for both enqueue/dequeue.
// * PrologueQueue is ordered. It will not release task that enqueue later first.
// * PrologueQueue is specific for message-processing.
class PrologueQueue {
public:
    // Prologue callback should:
    // * take ownership of message. It is callback's responsibility to keep message alive
    //   before returning it.
    // * be stateless. Any processing logic that require mutating replica state should be
    //   done by replica sequencially.
    // * optionally decide whether the message is dropped. If callback drops the message,
    //   it returns nullptr instead of the message.
    using Prologue = std::function<void (AbstractPrologueTask &)>;
    PrologueQueue(int nb_thread);
    void Enqueue(std::unique_ptr<AbstractPrologueTask> task, Prologue prologue);
    std::unique_ptr<AbstractPrologueTask> Dequeue();
private:
    struct WorkingTask {
        std::future<void> handle;
        AbstractPrologueTask *data;
    };
    std::queue<WorkingTask> tasks;
    ctpl::thread_pool pool;
};

}