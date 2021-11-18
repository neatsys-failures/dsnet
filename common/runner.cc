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
static Latency_t driver_spin;


Runner::Runner(int nb_worker_thread) 
    : nb_worker_thread(nb_worker_thread), shutdown(false), solo_slot(0), next_slot(0)
{
    if (NB_SLOT_MAX < NB_SLOT) {
        Panic("Not enough slot");
    }
    for (int i = 0; i < nb_worker_thread; i += 1) {
        _Latency_Init(&worker_spin[i], "");
        _Latency_Init(&solo_spin[i], "");
        _Latency_Init(&job_total[i], "");
    }
    _Latency_Init(&driver_spin, "driver_spin");

    for (int i = 0; i < NB_SLOT; i += 1) {
        slot_ready[i] = true;
    }

    for (int i = 0; i < nb_worker_thread; i += 1) {
        worker_threads[i] = thread([this, i]() {
            RunWorkerThread(i);
        });
    }

    SetThreadAffinity(pthread_self(), 0);
    int cpu = 0;
    for (int i = 0; i < nb_worker_thread; i += 1) {
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
    for (int i = 0; i < nb_worker_thread; i += 1) {
        Latency_Sum(&sum_worker_spin, &worker_spin[i]);
        Latency_Sum(&sum_solo_spin, &solo_spin[i]);
        Latency_Sum(&sum_job_total, &job_total[i]);
    }

    Latency_Dump(&driver_spin);
    Latency_Dump(&sum_worker_spin);
    Latency_Dump(&sum_solo_spin);
    Latency_Dump(&sum_job_total);

    shutdown = true;
    for (int i = 0; i < nb_worker_thread; i += 1) {
        worker_threads[i].join();
    }
}

void Runner::RunWorkerThread(int worker_id) {
    int slot_id = worker_id;
    while (true) {
        Latency_Start(&job_total[worker_id]);
        Latency_Start(&worker_spin[worker_id]);
        while (slot_ready[slot_id]) {
            if (shutdown) {
                return;
            }
        }
        Latency_End(&worker_spin[worker_id]);
        Solo solo = prologue_slots[slot_id]();
        prologue_slots[slot_id] = nullptr;
        slot_ready[slot_id] = true;

        if (solo) {
            // TODO order
            Latency_Start(&solo_spin[worker_id]);
            while (solo_slot != slot_id) {
                if (shutdown) {
                    return;
                }
            }
            Latency_End(&solo_spin[worker_id]);
            solo();
            solo_slot = (solo_slot + 1) % NB_SLOT;
            if (epilogue_slots[slot_id]) {
                epilogue_slots[slot_id]();
                epilogue_slots[slot_id] = nullptr;
            }
        }
        slot_id = (slot_id + nb_worker_thread) % NB_SLOT;
        Latency_End(&job_total[worker_id]);
    }
}

void Runner::RunPrologue(Prologue prologue) {
    Latency_Start(&driver_spin);
    while (!slot_ready[next_slot]) {
        if (shutdown) {
            return;
        }
    }
    Latency_End(&driver_spin);

    prologue_slots[next_slot] = prologue;
    // epilogue_slots[next_slot] = nullptr;
    slot_ready[next_slot] = false;
    next_slot = (next_slot + 1) % NB_SLOT;
}

void Runner::RunEpilogue(Epilogue epilogue) {
    // Latency_Start(&solo_push);
    epilogue_slots[solo_slot] = epilogue;
    // Latency_End(&solo_push);
}

}