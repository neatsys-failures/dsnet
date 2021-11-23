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
    (void) status;
}

using std::thread;

Latency_t driver_spin, solo_spin, solo_task;
Latency_t worker_spin[WORKER_COUNT_MAX], worker_task[WORKER_COUNT_MAX];

Runner::Runner(int worker_thread_count) 
    : worker_thread_count(worker_thread_count), shutdown(false)
{
    if (worker_thread_count > WORKER_COUNT_MAX) {
        Panic("Too many worker");
    }

    _Latency_Init(&driver_spin, "driver_spin");
    _Latency_Init(&solo_spin, "solo_spin");
    _Latency_Init(&solo_task, "solo_task");
    for (int i = 0; i < worker_thread_count; i += 1) {
        _Latency_Init(&worker_spin[i], "");
        _Latency_Init(&worker_task[i], "");
    }

    for (int i = 0; i < worker_thread_count; i += 1) {
        idle_hint[i] = true;
    }
    last_idle = 0;
    working_solo = 0;
    for (int i = 0; i < SOLO_RING_SIZE; i += 1) {
        pending_solo[i] = false;
    }
    last_epilogue = 0;
    for (int i = 0; i < EPILOGUE_RING_SIZE; i += 1) {
        pending_epilogue[i] = false;
    }
    last_task = 0;

    for (int i = 0; i < worker_thread_count; i += 1) {
        worker_threads[i] = thread([this, i]() {
            RunWorkerThread(i);
        });
    }
    solo_thread = thread([this]() {
        RunSoloThread();
    });
    epilogue_thread = thread([this]() {
        RunEpilogueThread();
    });

    SetThreadAffinity(pthread_self(), 0);
    SetThreadAffinity(solo_thread.native_handle(), 1);
    SetThreadAffinity(epilogue_thread.native_handle(), 2);
    int cpu = 2;
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
#ifndef DSNET_RUNNER_ALLOW_DISDARD
    // user should make sure no more RunPrologue calling
    // or this can block forever
    while (working_solo <= last_task || last_epilogue < working_solo - 1) {
    }
#endif

    shutdown = true;
    for (int i = 0; i < worker_thread_count; i += 1) {
        worker_threads[i].join();
    }
    solo_thread.join();
    epilogue_thread.join();

    Latency_t sum_worker_spin, sum_worker_task;
    _Latency_Init(&sum_worker_spin, "worker_spin");
    _Latency_Init(&sum_worker_task, "worker_task");
    for (int i = 0; i < worker_thread_count; i += 1) {
        Latency_Sum(&sum_worker_spin, &worker_spin[i]);
        Latency_Sum(&sum_worker_task, &worker_task[i]);
    }
    Latency_Dump(&driver_spin);
    Latency_Dump(&solo_spin);
    Latency_Dump(&sum_worker_spin);
    Latency_Dump(&solo_task);
    Latency_Dump(&sum_worker_task);
}

auto Runner::PopEpilogue() -> Epilogue {
    int order = last_epilogue + 1;
    Assert(order <= working_solo);
    if (order == working_solo) {
        return nullptr;
    }
    int t = order - 1;
    if (!last_epilogue.compare_exchange_strong(t, order)) {
        return nullptr;
    }
    int slot = order % EPILOGUE_RING_SIZE;
    if (!pending_epilogue[slot]) {
        return nullptr;
    }
    Epilogue epilogue = epilogue_ring[slot];
    epilogue_ring[slot] = nullptr;
    pending_epilogue[slot] = false;
    return epilogue;
}

void Runner::RunWorkerThread(int worker_id) {
    while (true) {
        Latency_Start(&worker_spin[worker_id]);
        while (idle_hint[worker_id]) {
            if (shutdown) {
                return;
            }
        }
        Latency_End(&worker_spin[worker_id]);
        Latency_Start(&worker_task[worker_id]);
        Prologue prologue = working_prologue[worker_id];
        int order = task_order[worker_id];
        working_prologue[worker_id] = nullptr;
        // is it ok?
        idle_hint[worker_id] = true;
        last_idle = worker_id;

        Solo solo = prologue();
        if (order - working_solo >= SOLO_RING_SIZE) {
            Panic("Solo ring overflow");
        }
        Debug("Insert solo: order = %d", order);
        int slot = order % SOLO_RING_SIZE;
        Assert(!pending_solo[slot]);
        solo_ring[slot] = solo;
        pending_solo[slot] = true;

        Epilogue epilogue = PopEpilogue();
        if (epilogue) {
            epilogue();
            Latency_EndType(&worker_task[worker_id], 'e');
        } else {
            Latency_End(&worker_task[worker_id]);
        }
        // idle_hint[worker_id] = true;
    }
}

void Runner::RunSoloThread() {
    while (true) {
        Latency_Start(&solo_spin);
        working_solo += 1;
        // Debug("solo pending: working = %d", (int) working_solo);
        int slot = working_solo % SOLO_RING_SIZE;
        while (!pending_solo[slot]) {
            if (shutdown) {
                return;
            }
        }
        Latency_End(&solo_spin);
        Latency_Start(&solo_task);
        Solo solo = solo_ring[slot];
        solo_ring[slot] = nullptr;
        pending_solo[slot] = false;

        if (solo) {
            solo();
        }
        Latency_End(&solo_task);
    }
}

void Runner::RunEpilogueThread() {
    while (!shutdown) {
        Epilogue epilogue = PopEpilogue();
        if (epilogue) {
            epilogue();
        }
    }
}

void Runner::RunPrologue(Prologue prologue) {
    last_task += 1;
    Latency_Start(&driver_spin);
    while (last_task > working_solo + SOLO_RING_SIZE) {
        if (shutdown) {
            return;
        }
    }
    Latency_EndType(&driver_spin, 's');
    Latency_Start(&driver_spin);
    // int worker_id = last_task % worker_thread_count;
    // int worker_id = last_idle;
    static int worker_id;
    while (!idle_hint[worker_id]) {
        if (shutdown) {
            return;
        }
        worker_id = (worker_id + 1) % worker_thread_count;
    }
    Latency_EndType(&driver_spin, 'w');
    Debug("Dispatch: task = %d, worker = %d", last_task, worker_id);
    working_prologue[worker_id] = prologue;
    task_order[worker_id] = last_task;
    idle_hint[worker_id] = false;
}

void Runner::RunEpilogue(Epilogue epilogue) {
    int slot = working_solo % EPILOGUE_RING_SIZE;
    if (pending_epilogue[slot]) {
        Panic("Epilogue overflow");
    }
    epilogue_ring[slot] = epilogue;
    pending_epilogue[slot] = true;
}

}