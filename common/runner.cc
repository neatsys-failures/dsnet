#include "common/runner.h"
#include "lib/assert.h"
#include "lib/latency.h"
#include "lib/message.h"
#include <future>
#include <pthread.h>

namespace dsnet {

static void SetThreadAffinity(pthread_t thread, int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    int status = pthread_setaffinity_np(thread, sizeof(mask), &mask);
    // local dev env don't have that many cores >_<
    // ASSERT(status == 0);
    (void)status;
}

using std::thread;

Latency_t driver_spin, solo_spin, solo_task;
Latency_t worker_spin[N_WORKER_MAX], worker_task[N_WORKER_MAX],
    unstable_epilogue[N_WORKER_MAX];

Runner::Runner(int n_worker) : shutdown(false), n_worker(n_worker) {
    if (n_worker > N_WORKER_MAX) {
        Panic("Too many worker");
    }

    _Latency_Init(&driver_spin, "driver_spin");
    _Latency_Init(&solo_spin, "solo_spin");
    _Latency_Init(&solo_task, "solo_task");
    for (int i = 0; i < n_worker; i += 1) {
        _Latency_Init(&worker_spin[i], "");
        _Latency_Init(&worker_task[i], "");
        _Latency_Init(&unstable_epilogue[i], "");
    }

    for (int i = 0; i < n_worker; i += 1) {
        worker_idle[i] = true;
    }
    idle_hint = 0;
    last_solo = 0;
    prologue_id = 0;
    working_epilogue = nullptr;

    for (int i = 0; i < n_worker; i += 1) {
        workers[i] = thread([this, i]() { RunWorkerThread(i); });
    }

    SetThreadAffinity(pthread_self(), 0);
    int cpu = 0;
    for (int i = 0; i < n_worker; i += 1) {
        cpu += 1;
        while (cpu == 15 || cpu / 16 == 1) {
            cpu += 1;
        }
        if (cpu == 64) {
            Panic("Too many worker threads");
        }
        SetThreadAffinity(workers[i].native_handle(), cpu);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

Runner::~Runner() {
    shutdown = true;
    for (int i = 0; i < n_worker; i += 1) {
        workers[i].join();
    }

    Latency_t sum_worker_spin, sum_worker_task, sum_unstable_epilogue;
    _Latency_Init(&sum_worker_spin, "worker_spin");
    _Latency_Init(&sum_worker_task, "worker_task");
    _Latency_Init(&sum_unstable_epilogue, "unstable_epilogue");
    for (int i = 0; i < n_worker; i += 1) {
        Latency_Sum(&sum_worker_spin, &worker_spin[i]);
        Latency_Sum(&sum_worker_task, &worker_task[i]);
        Latency_Sum(&sum_unstable_epilogue, &unstable_epilogue[i]);
    }
    Latency_Dump(&driver_spin);
    Latency_Dump(&solo_spin);
    Latency_Dump(&sum_worker_spin);
    Latency_Dump(&solo_task);
    Latency_Dump(&sum_worker_task);
    Latency_Dump(&sum_unstable_epilogue);
}

void Runner::RunWorkerThread(int worker_id) {
    while (true) {
        Latency_Start(&worker_spin[worker_id]);
        while (worker_idle[worker_id]) {
            if (shutdown) {
                return;
            }
            idle_hint = worker_id;
        }
        Latency_EndType(&worker_spin[worker_id], 'p');
        Prologue prologue = working_prologue[worker_id].task;
        int task_id = working_prologue[worker_id].id;
        worker_idle[worker_id] = true;

        Latency_Start(&worker_task[worker_id]);
        Solo solo = prologue();
        Latency_EndType(&worker_task[worker_id], 'p');
        if (!solo) {
            continue;
        }
        Latency_Start(&worker_spin[worker_id]);
        while (last_solo != task_id - 1) {
            if (shutdown) {
                return;
            }
        }
        Latency_EndType(&worker_spin[worker_id], 's');
        Latency_Start(&worker_task[worker_id]);
        solo();
        Latency_EndType(&worker_task[worker_id], 's');
        Epilogue epilogue = working_epilogue;
        working_epilogue = nullptr;
        last_solo = task_id;

        if (epilogue) {
            Latency_Start(&worker_task[worker_id]);
            epilogue();
            Latency_EndType(&worker_task[worker_id], 'e');
        }
    }
}

void Runner::RunPrologue(Prologue prologue) {
#ifdef DSNET_SEQUENTIAL_RUNNER
    Solo solo = prologue();
    if (solo) {
        solo();
        if (working_epilogue) {
            working_epilogue();
            working_epilogue = nullptr;
        }
    }
    return;
#endif
    prologue_id += 1;
    int worker_id = idle_hint;
    // this loop happens in the same thread of ~Runner
    // so if no worker ever finish, there is no chance to set `shutdown`
    // some hard timeout may be necessary
    Latency_Start(&driver_spin);
    while (!worker_idle[worker_id]) {
        worker_id = idle_hint;
    }
    Latency_End(&driver_spin);
    working_prologue[worker_id].task = prologue;
    working_prologue[worker_id].id = prologue_id;
    worker_idle[worker_id] = false;
}

void Runner::RunEpilogue(Epilogue epilogue) {
    ASSERT(!working_epilogue);
    working_epilogue = epilogue;
}

} // namespace dsnet