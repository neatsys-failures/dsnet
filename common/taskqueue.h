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

template<typename M> class PrologueTaskSpec;  // specialization
class PrologueTask {
public:
    virtual bool HasMessage() const = 0;
    virtual const TransportAddress &Remote() const = 0;
    virtual void ParseWithAdapter(class Message &adapter) const = 0;
    virtual ~PrologueTask() {
    }
    template<typename M> static PrologueTask *New(
        void *buf, size_t len, const TransportAddress &remote_addr) 
    {
        return new PrologueTaskSpec<M>(buf, len, remote_addr);
    }
    // if prologue did not call SetMessage during preprocessing, the task will be
    // quitely dropped by PrologueQueue without delivering it to Dequeue
    template<typename M> void SetMessage(std::unique_ptr<M> message) {
        dynamic_cast<PrologueTaskSpec<M> &>(*this).message = move(message);
    }
    template<typename M> M &Message() {
        ASSERT(HasMessage());
        return *dynamic_cast<PrologueTaskSpec<M> &>(*this).message;
    }
};

// General parallization for message-preprocessing.
// Compare to native worker thread pool:
// * PrologueQueue is passive. It relies on external calling for both enqueue/dequeue.
// * PrologueQueue is ordered. It will not release task that enqueue later first.
// * PrologueQueue is specialized for message-processing.
class PrologueQueue {
public:
    // Prologue callback should:
    // * take ownership of message. It is callback's responsibility to keep message alive
    //   before returning it.
    // * be stateless. Any processing logic that require mutating replica state should be
    //   done by replica sequencially.
    // * optionally decide whether the message is dropped. If callback drops the message,
    //   it returns nullptr instead of the message.
    using Prologue = std::function<void (PrologueTask &)>;
    PrologueQueue(int nb_thread);
    void Enqueue(std::unique_ptr<PrologueTask> task, Prologue prologue);
    std::unique_ptr<PrologueTask> Dequeue();
private:
    struct WorkingTask {
        std::future<void> handle;
        PrologueTask *data;
    };
    std::queue<WorkingTask> tasks;
    ctpl::thread_pool pool;
};

// private part
template<typename M> class PrologueTaskSpec : public PrologueTask {
    friend class PrologueTask;
private:
    unsigned char *buf;
    size_t len;
    const TransportAddress *remote;
    std::unique_ptr<M> message;

    bool HasMessage() const override {
        return message != nullptr;
    }
    PrologueTaskSpec(void *buf, size_t len, const TransportAddress &remote_addr)
        : buf(new unsigned char[len]), len(len), remote(remote_addr.clone()), message(nullptr)
    {
        std::memcpy(this->buf, buf, len);
    }
    ~PrologueTaskSpec() {
        delete[] buf;
        delete remote;
    }
    const TransportAddress &Remote() const override {
        return *remote;
    }
    void ParseWithAdapter(class Message &adapter) const override {
        adapter.Parse(buf, len);
    }
};

}