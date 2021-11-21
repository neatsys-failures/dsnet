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

using std::thread;

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
        slot_state[i] = SLOT_AVAIL;
        solo_next_slot[i] = -1;
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
        slot_id = (slot_id + worker_thread_count) % SLOT_COUNT;

        Latency_Start(&prologue_work[worker_id]);
        if (slot_state[slot_id] & SLOT_PENDING_EPILOGUE) {
            ASSERT(epilogue_slots[slot_id]);
            Debug("work on epilogue: slot = %d, state = %u", slot_id, (uint32_t) slot_state[slot_id]);
            epilogue_slots[slot_id]();
            epilogue_slots[slot_id] = nullptr;
            slot_state[slot_id] &= ~SLOT_PENDING_EPILOGUE;
            Latency_EndType(&prologue_work[worker_id], 'e');
        } else {
            // drop current recording
        }
        if (!(slot_state[slot_id] & SLOT_PENDING_PROLOGUE)) {
            continue;
        }

        // Latency_End(&worker_spin[worker_id]);
        Debug("work on prologue: slot = %d", slot_id);
        ASSERT(prologue_slots[slot_id]);
        Latency_Start(&prologue_work[worker_id]);
        Solo solo = prologue_slots[slot_id]();
        prologue_slots[slot_id] = nullptr;
        slot_state[slot_id] &= ~SLOT_PENDING_PROLOGUE;
        Latency_End(&prologue_work[worker_id]);

        Latency_Start(&solo_spin[worker_id]);
        while (slot_state[slot_id] & SLOT_PENDING_SOLO) {  // previous solo still in slot
            if (shutdown) {
                Latency_EndType(&solo_spin[worker_id], 's');
                return;
            }
        }
        Latency_End(&solo_spin[worker_id]);
        ASSERT(!solo_slots[slot_id]);
        solo_slots[slot_id] = solo;
        slot_state[slot_id] |= SLOT_PENDING_SOLO;
    }
}

void Runner::RunSoloThread() {
    solo_id = 0;
    while (true) {
        Latency_Start(&solo_idle);
        while (!(slot_state[solo_id] & SLOT_HAS_NEXT)) {
            if (shutdown) {
                Latency_EndType(&solo_idle, 's');
                return;
            }
        }
        int prev_slot = solo_id;
        solo_id = solo_next_slot[solo_id];
        // Debug("solo: %d -> %d", prev_slot, solo_id);
        solo_next_slot[prev_slot] = -1;
        slot_state[prev_slot] &= ~SLOT_HAS_NEXT;
        while (!(slot_state[solo_id] & SLOT_PENDING_SOLO)) {
            if (shutdown) {
                Latency_EndType(&solo_idle, 's');
                return;
            }
        }
        Latency_End(&solo_idle);
        // Debug("solo: slot = %d", solo_id);
        Latency_Start(&solo_work);
        if (solo_slots[solo_id]) {
            solo_slots[solo_id]();
        }
        solo_slots[solo_id] = nullptr;
        slot_state[solo_id] &= ~SLOT_PENDING_SOLO;
        Latency_End(&solo_work);
    }
}

void Runner::RunPrologue(Prologue prologue) {
    Latency_Start(&driver_spin);
    int prev_slot = next_slot;
    next_slot = (next_slot + 1) % SLOT_COUNT;
    while (true) {
        if (shutdown) {
            Latency_EndType(&driver_spin, 's');
            return;
        }
        if (next_slot != prev_slot && !(slot_state[next_slot] & SLOT_PENDING_PROLOGUE)) {
            break;
        }
        next_slot = (next_slot + 1) % SLOT_COUNT;
    }
    Debug("alloc slot = %d", next_slot);
    Latency_End(&driver_spin);
    prologue_slots[next_slot] = prologue;
    slot_state[next_slot] |= SLOT_PENDING_PROLOGUE;
    solo_next_slot[prev_slot] = next_slot;
    slot_state[prev_slot] |= SLOT_HAS_NEXT;
}

void Runner::RunEpilogue(Epilogue epilogue) {
    // invariant: epilogue slot must be available at this time
    Debug("pending epilogue: slot = %d", solo_id);
    epilogue_slots[solo_id] = epilogue;
    slot_state[solo_id] |= SLOT_PENDING_EPILOGUE;
}

}