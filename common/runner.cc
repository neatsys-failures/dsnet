#include "common/runner.h"
#include "lib/assert.h"
#include "lib/latency.h"
#include "lib/message.h"
#include <future>
#include <pthread.h>

namespace dsnet {

static int cpus[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,-1};

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

static Latency_t worker_spin[128], solo_spin[128], prologue_work[128];
static Latency_t driver_spin, solo_idle, solo_work;

Runner::Runner(int worker_thread_count) 
    : worker_thread_count(worker_thread_count), shutdown(false), solo_id(0), next_slot(0)
{
    if (SLOT_COUNT_MAX < SLOT_COUNT) {
        Panic("Not enough slot");
    }
    for (int i = 0; i < worker_thread_count; i += 1) {
        _Latency_Init(&worker_spin[i], "");
        _Latency_Init(&solo_spin[i], "");
        _Latency_Init(&prologue_work[i], "");
    }
    _Latency_Init(&driver_spin, "driver_spin");
    _Latency_Init(&solo_idle, "solo_idle");
    _Latency_Init(&solo_work, "solo_work");

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

    SetThreadAffinity(pthread_self(), cpus[0]);
    SetThreadAffinity(solo_thread.native_handle(), cpus[1]);
    int cpu_i = 1;
    for (int i = 0; i < worker_thread_count; i += 1) {
        cpu_i += 1;
        if (cpus[cpu_i] == -1) {
            Panic("Too many worker threads");
        }
        SetThreadAffinity(worker_threads[i].native_handle(), cpus[cpu_i]);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

Runner::~Runner() {
    Latency_t sum_worker_spin, sum_solo_spin, sum_prologue_work;
    _Latency_Init(&sum_worker_spin, "worker_spin");
    _Latency_Init(&sum_solo_spin, "solo_spin");
    _Latency_Init(&sum_prologue_work, "prologue_work");
    for (int i = 0; i < worker_thread_count; i += 1) {
        Latency_Sum(&sum_worker_spin, &worker_spin[i]);
        Latency_Sum(&sum_solo_spin, &solo_spin[i]);
        Latency_Sum(&sum_prologue_work, &prologue_work[i]);
    }

    Latency_Dump(&driver_spin);
    Latency_Dump(&solo_idle);
    Latency_Dump(&solo_work);
    Latency_Dump(&sum_worker_spin);
    Latency_Dump(&sum_solo_spin);
    Latency_Dump(&sum_prologue_work);

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

        // Latency_End(&worker_spin[worker_id]);
        Latency_Start(&prologue_work[worker_id]);
        Solo solo = prologue_slots[slot_id]();
        prologue_slots[slot_id] = nullptr;
        slot_ready[slot_id] = true;
        Latency_End(&prologue_work[worker_id]);

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
        Latency_Start(&solo_work);
        if (solo_slots[solo_id]) {
            solo_slots[solo_id]();
        }
        solo_slots[solo_id] = nullptr;
        pending_solo[solo_id] = false;
        Latency_End(&solo_work);
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