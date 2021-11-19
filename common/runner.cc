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
    // ASSERT(status == 0);
}

using std::promise;
using std::move;
using std::future;
using std::future_error;
using std::thread;
using std::unique_lock;
using std::mutex;
using std::atomic;

static Latency_t worker_spin[128], solo_spin[128], job_total[128];
static Latency_t driver_spin, solo_idle;

Runner::Runner(int worker_thread_count) 
    : worker_thread_count(worker_thread_count), shutdown(false), solo_id(0), next_slot(0)
{
    if (SLOT_COUNT_MAX < SLOT_COUNT) {
        Panic("Not enough slot");
    }
    for (int i = 0; i < worker_thread_count; i += 1) {
        _Latency_Init(&worker_spin[i], "");
        _Latency_Init(&solo_spin[i], "");
        _Latency_Init(&job_total[i], "");
    }
    _Latency_Init(&driver_spin, "driver_spin");
    _Latency_Init(&solo_idle, "solo_idle");

    for (int i = 0; i < SLOT_COUNT; i += 1) {
        slot_ready[i] = true;
        pending_epilogue[i] = false;
    }

    for (int i = 0; i < worker_thread_count; i += 1) {
        worker_threads[i] = thread([this, i]() {
            RunWorkerThread(i);
        });
    }
    solo_thread = thread([this]() {
        RunSoloThread();
    });

    SetThreadAffinity(pthread_self(), 0);
    SetThreadAffinity(solo_thread.native_handle(), 1);
    int cpu = 1;
    for (int i = 0; i < worker_thread_count; i += 1) {
        cpu += 1;
        while (cpu == 15 || cpu == 32 || cpu == 33) {
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
    Latency_t sum_worker_spin, sum_solo_spin, sum_job_total;
    _Latency_Init(&sum_worker_spin, "worker_spin");
    _Latency_Init(&sum_solo_spin, "solo_spin");
    _Latency_Init(&sum_job_total, "job_total");
    for (int i = 0; i < worker_thread_count; i += 1) {
        Latency_Sum(&sum_worker_spin, &worker_spin[i]);
        Latency_Sum(&sum_solo_spin, &solo_spin[i]);
        Latency_Sum(&sum_job_total, &job_total[i]);
    }

    Latency_Dump(&driver_spin);
    Latency_Dump(&solo_idle);
    Latency_Dump(&sum_worker_spin);
    Latency_Dump(&sum_solo_spin);
    Latency_Dump(&sum_job_total);

    shutdown = true;
    for (int i = 0; i < worker_thread_count; i += 1) {
        worker_threads[i].join();
    }
    solo_thread.join();
}

void Runner::RunWorkerThread(int worker_id) {
    int slot_id = worker_id - worker_thread_count;
    while (!shutdown) {
        // Latency_Start(&job_total[worker_id]);
        slot_id = (slot_id + worker_thread_count) % SLOT_COUNT;

        Latency_Start(&solo_spin[worker_id]);
        while (pending_solo[slot_id]) {
            if (shutdown) {
                return;
            }
        }
        if (pending_epilogue[slot_id]) {
            Latency_EndType(&solo_spin[worker_id], 'e');
            epilogue_slots[slot_id]();
            epilogue_slots[slot_id] = nullptr;
            pending_epilogue[slot_id] = false;
        } else {
            // drop current recording
            // Latency_EndType(&solo_spin[worker_id], '_');
        }
        if (slot_ready[slot_id]) {  // slot ready = no work
            continue;
        }

        // Latency_Start(&worker_spin[worker_id]);
        // while (slot_ready[slot_id]) {
        //     if (shutdown) {
        //         return;
        //     }
        // }
        // Latency_End(&worker_spin[worker_id]);
        Solo solo = prologue_slots[slot_id]();
        prologue_slots[slot_id] = nullptr;
        slot_ready[slot_id] = true;

        Latency_Start(&solo_spin[worker_id]);
        while (pending_solo[slot_id]) {  // previous solo still in slot
            if (shutdown) {
                Latency_EndType(&solo_spin[worker_id], 's');
                return;
            }
        }
        Latency_End(&solo_spin[worker_id]);
        solo_slots[slot_id] = solo;
        pending_solo[slot_id] = true;

        // if (solo) {
        //     // TODO order
        //     while (solo_slot != slot_id) {
        //         if (shutdown) {
        //             return;
        //         }
        //     }
        //     solo();
        //     solo_slot = (solo_slot + 1) % NB_SLOT;
        //     if (epilogue_slots[slot_id]) {
        //         epilogue_slots[slot_id]();
        //         epilogue_slots[slot_id] = nullptr;
        //     }
        // }
        // slot_id = (slot_id + nb_worker_thread) % NB_SLOT;
        // Latency_End(&job_total[worker_id]);
    }
}

void Runner::RunSoloThread() {
    for (solo_id = 0;; solo_id = (solo_id + 1) % SLOT_COUNT) {
        Latency_Start(&solo_idle);
        while (!pending_solo[solo_id]) {
            if (shutdown) {
                Latency_EndType(&solo_idle, 's');
                return;
            }
        }
        Latency_End(&solo_idle);
        if (solo_slots[solo_id]) {
            solo_slots[solo_id]();
        }
        solo_slots[solo_id] = nullptr;
        pending_solo[solo_id] = false;
    }
}

void Runner::RunPrologue(Prologue prologue) {
    Latency_Start(&driver_spin);
    while (!slot_ready[next_slot]) {
        if (shutdown) {
            Latency_EndType(&driver_spin, 's');
            return;
        }
    }
    Latency_End(&driver_spin);

    prologue_slots[next_slot] = prologue;
    slot_ready[next_slot] = false;
    next_slot = (next_slot + 1) % SLOT_COUNT;
}

void Runner::RunEpilogue(Epilogue epilogue) {
    // invariant: epilogue slot must be available at this time
    epilogue_slots[solo_id] = epilogue;
    pending_epilogue[solo_id] = true;
}

}