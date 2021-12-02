#include "common/runner.h"
#include "lib/assert.h"
#include "lib/latency.h"
#include <pthread.h>

namespace dsnet {

using std::thread;

#define N_WORKER_MAX 128
Latency_t driver_spin, solo_spin, solo_task;
Latency_t worker_spin[N_WORKER_MAX], worker_task[N_WORKER_MAX],
    unstable_epilogue[N_WORKER_MAX];
Latency_t sum_worker_task, sum_worker_spin;

static void SetThreadAffinity(pthread_t thread, int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    int status = pthread_setaffinity_np(thread, sizeof(mask), &mask);
    // local dev env don't have that many cores >_<
    // ASSERT(status == 0);
    (void)status;
}

Runner::Runner() {
    SetThreadAffinity(pthread_self(), 0);
    core_id = 0;

    _Latency_Init(&driver_spin, "driver_spin");
    for (int i = 0; i < N_WORKER_MAX; i += 1) {
        _Latency_Init(&worker_task[i], "");
    }
    _Latency_Init(&sum_worker_task, "worker_task");
    for (int i = 0; i < N_WORKER_MAX; i += 1) {
        _Latency_Init(&worker_spin[i], "");
    }
    _Latency_Init(&sum_worker_spin, "worker_spin");
}

Runner::~Runner() {
    Latency_Dump(&driver_spin);
    for (int i = 0; i < N_WORKER_MAX; i += 1) {
        Latency_Sum(&sum_worker_task, &worker_task[i]);
    }
    Latency_Dump(&sum_worker_task);
    for (int i = 0; i < N_WORKER_MAX; i += 1) {
        Latency_Sum(&sum_worker_spin, &worker_spin[i]);
    }
    Latency_Dump(&sum_worker_spin);
}

void Runner::SetAffinity(thread &t) {
    core_id += 1;
    while (core_id == 15 || core_id / 16 == 1) {
        core_id += 1;
    }
    if (core_id == 64) {
        Panic("Too many threads");
    }
    SetThreadAffinity(t.native_handle(), core_id);
}

void CTPLRunner::RunPrologue(Prologue prologue) {
    Latency_Start(&driver_spin);
    pool.push([this, prologue](int id) {
        Latency_Start(&worker_task[id]);
        Solo solo = prologue();
        Latency_EndType(&worker_task[id], 'p');
        if (!solo) {
            return;
        }

        Latency_Start(&worker_spin[id]);
        std::unique_lock<std::mutex> replica_lock(replica_mutex);
        Latency_EndType(&worker_spin[id], 's');

        Latency_Start(&worker_task[id]);
        this->epilogue = nullptr;
        solo();
        Latency_EndType(&worker_task[id], 's');

        Epilogue epilogue = this->epilogue;
        replica_lock.unlock();
        if (epilogue) {
            Latency_Start(&worker_task[id]);
            epilogue();
            Latency_EndType(&worker_task[id], 'e');
        } else {
        }
    });
    Latency_End(&driver_spin);
}

} // namespace dsnet