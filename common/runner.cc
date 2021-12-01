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
using std::unique_ptr;

Latency_t driver_spin, solo_spin, solo_task;
Latency_t worker_spin[N_WORKER_MAX], worker_task[N_WORKER_MAX],
    unstable_epilogue[N_WORKER_MAX];

Runner::Runner(int n_worker, bool seq_solo)
    : n_worker(n_worker), seq_solo(seq_solo), shutdown(false), solo_queue(256),
      prologue_queue(256), epilogue_queue(256) {
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

    for (int i = 0; i < SOLO_RING_SIZE; i += 1) {
        pending_solo[i] = false;
    }
    last_task = 0;

    for (int i = 0; i < n_worker; i += 1) {
        worker_threads[i] = thread([this, i]() { RunWorkerThread(i); });
    }
    solo_thread = thread([this]() { RunSoloThread(); });
    epilogue_thread = thread([this]() { RunEpilogueThread(true, -1); });

    SetThreadAffinity(pthread_self(), 0);
    SetThreadAffinity(solo_thread.native_handle(), 1);
    SetThreadAffinity(epilogue_thread.native_handle(), 2);
    int cpu = 2;
    for (int i = 0; i < n_worker; i += 1) {
        cpu += 1;
        while (cpu == 15 || cpu == 33 || cpu / 16 == 1) {
            cpu += 1;
        }
        if (cpu == 64) {
            Panic("Too many worker threads");
        }
        SetThreadAffinity(worker_threads[i].native_handle(), cpu);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

Runner::~Runner() {
    shutdown = true;
    for (int i = 0; i < n_worker; i += 1) {
        worker_threads[i].join();
    }
    solo_thread.join();
    epilogue_thread.join();

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
    while (!shutdown) {
        PrologueTask task;
        bool has_work = prologue_queue.pop(task);
        if (!has_work) {
            if (worker_id != 0) {
                RunEpilogueThread(false, worker_id);
            }
            continue;
        }

        Latency_Start(&worker_task[worker_id]);
        auto prologue = unique_ptr<Prologue>(task.prologue);
        int order = task.id;

        Solo solo = (*prologue)();

        if (seq_solo) {
            Debug("Insert solo: order = %d", order);
            int slot = order % SOLO_RING_SIZE;
            if (pending_solo[slot]) {
                Panic("Solo ring overflow");
            }
            solo_ring[slot] = solo;
            pending_solo[slot] = true;
        } else {
            solo_queue.push(new Solo(solo));
        }
        Latency_End(&worker_task[worker_id]);
    }
}

void Runner::RunSoloThread() {
    int slot = 0;
    while (true) {
        Solo solo;
        Latency_Start(&solo_spin);
        if (seq_solo) {
            slot = (slot + 1) % SOLO_RING_SIZE;
            while (!pending_solo[slot]) {
                if (shutdown) {
                    return;
                }
            }
            solo = solo_ring[slot];
            solo_ring[slot] = nullptr;
            pending_solo[slot] = false;
        } else {
            Solo *solo_ptr;
            while (!solo_queue.pop(solo_ptr)) {
                if (shutdown) {
                    return;
                }
            }
            solo = *solo_ptr;
            delete solo_ptr;
        }
        Latency_End(&solo_spin);

        Latency_Start(&solo_task);
        if (solo) {
            solo();
        }
        Latency_End(&solo_task);
    }
}

void Runner::RunEpilogueThread(bool stable, int worker_id) {
    if (!stable) {
        Latency_Start(&unstable_epilogue[worker_id]);
    }
    while (!shutdown) {
        Epilogue *epilogue;
        bool has_work = epilogue_queue.pop(epilogue);
        if (!has_work) {
            if (!stable) {
                Latency_End(&unstable_epilogue[worker_id]);
                return;
            }
            continue;
        }
        (*epilogue)();
        delete epilogue;
    }
}

void Runner::RunPrologue(Prologue prologue) {
    last_task += 1;
    PrologueTask task;
    task.id = last_task;
    task.prologue = new Prologue(prologue);
    Latency_Start(&driver_spin);
    bool r = prologue_queue.push(task);
    Latency_End(&driver_spin);
    if (!r) {
        Panic("Fail to push prologue");
    }
}

void Runner::RunEpilogue(Epilogue epilogue) {
    if (!epilogue_queue.push(new Epilogue(epilogue))) {
        Panic("Fail to push epilogue");
    }
}

} // namespace dsnet