#pragma once
#include "lib/ctpl.h"
#include <functional>
#include <thread>

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
    Runner(int worker_thread_count);
    ~Runner();

    using Solo = std::function<void ()>;
    using Prologue = std::function<Solo ()>;
    void RunPrologue(Prologue prologue);
    using Epilogue = std::function<void ()>;
    void RunEpilogue(Epilogue epilogue);
private:
    int worker_thread_count;
    std::thread worker_threads[128];
    std::thread solo_thread;
    std::atomic<bool> shutdown;

#define SLOT_COUNT (worker_thread_count * 32)
#define SLOT_COUNT_MAX 1000
    Prologue prologue_slots[SLOT_COUNT_MAX];
    Epilogue epilogue_slots[SLOT_COUNT_MAX];
    Solo solo_slots[SLOT_COUNT_MAX];
    std::atomic<bool> slot_ready[SLOT_COUNT_MAX];
    std::atomic<bool> pending_solo[SLOT_COUNT_MAX], pending_epilogue[SLOT_COUNT_MAX];
    int solo_id, next_slot;

    void RunWorkerThread(int worker_id);
    void RunSoloThread();
};

}
