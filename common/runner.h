#pragma once
#include "lib/ctpl.h"
#include <functional>
#include <thread>
#include <condition_variable>
#include <queue>
#include <mutex>

namespace dsnet {

// A runner designed for replication protocol.
//
// Pakcet-processing is modeled as a 3-stage task, where the 1st and 3rd stages
// are stateless and can be parallized arbitrarily, but the 2nd stage is stateful
// and must be sequentially exeucted on the same thread. The 3 stages are named
// prologue, solo and epilogue.
// Runner is responsible for:
// * keep track of message receiving order, and process solo stage in the same order
// * monitor solo stage throughput, and "behave" an equal overall throughput
// Specific speaking, when pushing packet into prologue stage, runner will keep
// blocking until enough number of packets finish solo part. In another word, runner
// keeps a upper bound of packets in prologue or solo stages.
class Runner {
public:
    Runner(int nb_worker_thread);
    ~Runner();

    using Solo = std::function<void ()>;
    using Prologue = std::function<Solo ()>;
    void RunPrologue(Prologue prologue);
    using Epilogue = std::function<void ()>;
    void RunEpilogue(Epilogue epilogue);
private:
    int nb_worker_thread;

    std::thread worker_threads[128];
    std::thread solo_thread, epilogue_thread;
    bool shutdown;

#define NB_CONCURRENT_TASK (56 * 4)
    std::promise<void> solo_owned[NB_CONCURRENT_TASK];
    std::promise<void> available[NB_CONCURRENT_TASK];

    Prologue prologue_jobs[NB_CONCURRENT_TASK];
    std::mutex prologue_mutexs[NB_CONCURRENT_TASK];
    Solo solo_jobs[NB_CONCURRENT_TASK];

    Epilogue epilogue_jobs[NB_CONCURRENT_TASK];
    std::mutex epilogue_mutexs[NB_CONCURRENT_TASK];

    int job_id, solo_job_id;
    void RunWorkerThread(int worker_id);
    void RunSoloThread();
    void RunEpilogueThread();
};

}
