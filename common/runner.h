#pragma once
#include "lib/ctpl.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace dsnet {

class Runner {
public:
    Runner();
    virtual ~Runner();

    using Solo = std::function<void()>;
    using Prologue = std::function<Solo()>;
    using Epilogue = std::function<void()>;

    virtual void RunPrologue(Prologue prologue) = 0;
    virtual void RunEpilogue(Epilogue epilogue) = 0;

protected:
    void SetAffinity(std::thread &t);

private:
    int core_id;
};

// * self-document of Runner model
// * single-threaded executor for debugging
// * measure the overhead introduced by Runner API
class NoRunner : public Runner {
    Epilogue epilogue;

public:
    void RunPrologue(Prologue prologue) override {
        Solo solo = prologue();
        if (!solo) {
            return;
        }
        epilogue = nullptr;
        solo();
        if (epilogue) {
            epilogue();
        }
    }
    void RunEpilogue(Epilogue epilogue) override { this->epilogue = epilogue; }
};

class CTPLRunner : public Runner {
    ctpl::thread_pool pool;
    std::mutex replica_mutex;
    Epilogue epilogue;

public:
    CTPLRunner(int n_worker) : pool(n_worker) {
        for (int i = 0; i < n_worker; i += 1) {
            SetAffinity(pool.get_thread(i));
        }
    }

    void RunPrologue(Prologue prologue) override;
    void RunEpilogue(Epilogue epilogue) override { this->epilogue = epilogue; }
};

class CTPLOrderedRunner : public Runner {
    ctpl::thread_pool pool;
    std::mutex replica_mutex;
    std::condition_variable cv;
    int prologue_id, last_task;
    Epilogue epilogue;

public:
    CTPLOrderedRunner(int n_worker) : pool(n_worker) {
        prologue_id = last_task = 0;

        for (int i = 0; i < n_worker; i += 1) {
            SetAffinity(pool.get_thread(i));
        }
    }

    void RunPrologue(Prologue prologue) override;
    void RunEpilogue(Epilogue epilogue) override { this->epilogue = epilogue; }
};

// Problem of pipeline model: replica thread spends lot of time pushing epilogue
// need to use with a non-blocking RunEpilogue
class CTPLPipelineRunner : public Runner {
    ctpl::thread_pool pool, replica_pool;

public:
    CTPLPipelineRunner(int n_worker) : pool(n_worker - 1), replica_pool(1) {
        SetAffinity(replica_pool.get_thread(0));
        for (int i = 0; i < n_worker - 1; i += 1) {
            SetAffinity(pool.get_thread(i));
        }
    }

    void RunPrologue(Prologue prologue) override;
    void RunEpilogue(Epilogue epilogue) override;
};

// seems to be fastest runner for now
// the downside of `SpinOrderedRunner` is that it uses a bounded queue
// (i.e. ring) buffer, and back-propagate blocking to `RunPrologue`
// caller if system overloaded. If client not slow down requesting soon
// enough, it could cause severe packet dropping
class SpinOrderedRunner : public Runner {
    int n_worker;
    static const int N_WORKER_MAX = 128;
    std::thread workers[N_WORKER_MAX];
    std::atomic<bool> shutdown;

    int n_slot() const { return n_worker * 4; }
    static const int N_SLOT_MAX = 1000;
    Prologue prologue_slots[N_SLOT_MAX];
    Epilogue epilogue_slots[N_SLOT_MAX];
    std::atomic<bool> slot_ready[N_SLOT_MAX];

    int next_prologue;
    std::atomic<int> next_solo;

    void RunWorkerThread(int id);

public:
    SpinOrderedRunner(int n_worker);
    ~SpinOrderedRunner();
    void RunPrologue(Prologue prologue) override;
    void RunEpilogue(Epilogue epilogue) override;
};

} // namespace dsnet