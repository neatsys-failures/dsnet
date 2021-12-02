#pragma once
#include "lib/ctpl.h"
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

} // namespace dsnet