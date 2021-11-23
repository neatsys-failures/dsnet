#pragma once
#include <atomic>
#include <functional>
#include <thread>

namespace dsnet {

// A runner designed for replication protocol.
//
// Pakcet-processing is modeled as a 3-stage task, where the 1st and 3rd stages
// are stateless and can be parallized arbitrarily, but the 2nd stage is stateful
// and must be sequentially exeucted, and the order of execution must match the 
// order of task-adding. 
// The 3 stages are named prologue, solo and epilogue. Solo and epilogue are optional.
// 
// Runner owns a set of worker threads, and try its best to schedule tasks, to
// accomplish the following objectives:
// * As long as there are enough number of workers, system overall throughput =
//   solo throughput
//   + "enough number" ideally => at least 
//     (prologue latency + epilogue latency) / solo latency
//     because of minor performance waving worker count should be a little bit more than it
// * Solo and epilogue has higher priority than incoming prologue. It is acceptable
//   to execute existing solo and epilogue in any order. As the result, the max
//   number of working task is bounded
// * When above two objectives are achieved minimize thread idle time, i.e. keep
//   needed worker count as close to ideal minimum as possible
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
#define WORKER_COUNT_MAX 128
#define SOLO_RING_SIZE 1000
#define EPILOGUE_RING_SIZE 1000

    int worker_thread_count;
    std::thread worker_threads[WORKER_COUNT_MAX], solo_thread, epilogue_thread;
    volatile bool shutdown;
    void RunWorkerThread(int worker_id);
    void RunSoloThread();
    void RunEpilogueThread();
    Epilogue PopEpilogue();

    std::atomic<bool> idle_hint[WORKER_COUNT_MAX];
    Prologue working_prologue[WORKER_COUNT_MAX];
    int task_order[WORKER_COUNT_MAX];

    Solo solo_ring[SOLO_RING_SIZE];
    std::atomic<int> working_solo;
    std::atomic<bool> pending_solo[SOLO_RING_SIZE];

    Epilogue epilogue_ring[EPILOGUE_RING_SIZE];
    std::atomic<bool> pending_epilogue[EPILOGUE_RING_SIZE];
    std::atomic<int> last_epilogue;

    int last_task;
};

}
