#pragma once
#include "lib/ctpl.h"
#include <functional>
#include <thread>
#include <condition_variable>
#include <queue>
#include <deque>
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
    std::atomic<bool> shutdown;

#define NB_SLOT (nb_worker_thread * 2)
#define NB_SLOT_MAX 1000
    Prologue prologue_slots[NB_SLOT_MAX];
    Epilogue epilogue_slots[NB_SLOT_MAX];
    std::atomic<bool> slot_ready[NB_SLOT_MAX];
    std::atomic<int> solo_slot;

    int next_slot;

    void RunWorkerThread(int worker_id);
};

}
