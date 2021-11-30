#pragma once
#include <atomic>
#include <functional>
#include <thread>

namespace dsnet {

// A runner designed for replication protocol.
//
// Pakcet-processing is modeled as a 3-stage task, where the 1st and 3rd stages
// are stateless and can be parallized arbitrarily, but the 2nd stage is
// stateful and must be sequentially exeucted, and the order of execution must
// match the order of task-adding. The 3 stages are named prologue, solo and
// epilogue. Solo and epilogue are optional.
//
// Runner owns a set of worker threads, and try its best to schedule tasks, to
// accomplish the following objectives:
// * As long as there are enough number of workers, system overall throughput =
//   solo throughput
//   + "enough number" ideally -> at least
//     (prologue latency + epilogue latency) / solo latency
//     because of minor performance waving worker count should be a little bit
//     more than it
// * Solo and epilogue has higher priority than incoming prologue. It is
// acceptable to execute existing solo and epilogue in any order. As the result,
//   + the max number of working task is bounded, no unnecessary queue overflow
//   + tail latency get best-effort control, which helps close-loop benchmark to
//   get full throughput
// * When above two objectives are achieved minimize thread idle time, i.e. keep
//   needed worker count as close to ideal minimum as possible
class Runner {
public:
    Runner(int worker_thread_count);
    ~Runner();

    using Solo = std::function<void()>;
    using Prologue = std::function<Solo()>;
    void RunPrologue(Prologue prologue);
    using Epilogue = std::function<void()>;
    void RunEpilogue(Epilogue epilogue);

private:
    std::atomic<bool> shutdown;

#define N_WORKER_MAX 128
    int n_worker;
    std::thread workers[N_WORKER_MAX];
    struct WorkingPrologue {
        Prologue task;
        int id;
    };
    WorkingPrologue working_prologue[N_WORKER_MAX];
    std::atomic<bool> worker_idle[N_WORKER_MAX];
    std::atomic<int> idle_hint;

    std::atomic<int> last_solo;
    Epilogue working_epilogue;

    int prologue_id;

    void RunWorkerThread(int worker_id);
};

} // namespace dsnet
