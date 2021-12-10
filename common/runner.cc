#include "common/runner.h"
#include "lib/assert.h"
#include "lib/latency.h"
#include <pthread.h>

namespace dsnet {

using std::mutex;
using std::thread;
using std::unique_lock;

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
        // while (core_id == 15) {
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
        unique_lock<mutex> replica_lock(replica_mutex);
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
        }
    });
    Latency_End(&driver_spin);
}

void CTPLOrderedRunner::RunPrologue(Prologue prologue) {
    prologue_id += 1;
    pool.push([this, prologue, prologue_id = prologue_id](int id) {
        Latency_Start(&worker_task[id]);
        Solo solo = prologue();
        Latency_EndType(&worker_task[id], 'p');

        Debug("Wait solo: worker id = %d, task id = %d", id, prologue_id);
        Latency_Start(&worker_spin[id]);
        unique_lock<mutex> replica_lock(replica_mutex);
        cv.wait(replica_lock, [this, prologue_id] {
            return last_task == prologue_id - 1;
        });
        Latency_EndType(&worker_spin[id], 's');
        Debug("Start solo: worker id = %d, task id = %d", id, prologue_id);

        this->epilogue = nullptr;
        if (solo) {
            Latency_Start(&worker_task[id]);
            solo();
            Latency_EndType(&worker_task[id], 's');
        }
        Debug("Done solo: worker id = %d, task id = %d", id, prologue_id);
        Epilogue epilogue = this->epilogue;
        last_task = prologue_id;
        replica_lock.unlock();
        cv.notify_all();

        if (epilogue) {
            Latency_Start(&worker_task[id]);
            epilogue();
            Latency_EndType(&worker_task[id], 'e');
        }
    });
}

void CTPLPipelineRunner::RunPrologue(Prologue prologue) {
    Latency_Start(&driver_spin);
    pool.push([this, prologue](int id) {
        Latency_Start(&worker_task[id + 1]);
        Solo solo = prologue();
        Latency_EndType(&worker_task[id + 1], 'p');

        if (solo) {
            Latency_Start(&worker_spin[id + 1]);
            replica_pool.push([this, solo](int id) {
                Latency_Start(&worker_task[0]);
                solo();
                Latency_EndType(&worker_task[0], 's');
            });
            Latency_EndType(&worker_spin[id + 1], 's');
        }
    });
    Latency_End(&driver_spin);
}

void CTPLPipelineRunner::RunEpilogue(Epilogue epilogue) {
    Latency_Start(&worker_spin[0]);
    pool.push([epilogue](int id) {
        Latency_Start(&worker_task[id + 1]);
        epilogue();
        Latency_EndType(&worker_task[id + 1], 'e');
    });
    Latency_EndType(&worker_spin[0], 'e');
}

SpinOrderedRunner::SpinOrderedRunner(int n_worker) : n_worker(n_worker) {
    if (n_worker > N_WORKER_MAX) {
        Panic("Too many workers");
    }
    if (n_slot() > N_SLOT_MAX) {
        Panic("Not enough slot");
    }

    for (int i = 0; i < n_slot(); i += 1) {
        slot_ready[i] = true;
    }
    next_prologue = next_solo = 0;

    for (int i = 0; i < n_worker; i += 1) {
        workers[i] = thread([this, i] { RunWorkerThread(i); });
        SetAffinity(workers[i]);
    }
}

SpinOrderedRunner::~SpinOrderedRunner() {
    shutdown = true;
    for (int i = 0; i < n_worker; i += 1) {
        workers[i].join();
    }
}

void SpinOrderedRunner::RunPrologue(Prologue prologue) {
    Latency_Start(&driver_spin);
    // it is unnecessary to check for `shutdown` during this spinning
    // since if driver thread is spinning here, no one will set `shutdown`
    // anyway
    DriverSpin();
    Latency_End(&driver_spin);

    prologue_slots[next_prologue] = prologue;
    slot_ready[next_prologue] = false;
    next_prologue = (next_prologue + 1) % n_slot();
}

void SpinOrderedRunner::RunEpilogue(Epilogue epilogue) {
    epilogue_slots[next_solo] = epilogue;
}

void SpinOrderedRunner::RunWorkerThread(int id) {
    int slot_id = id;
    while (true) {
        Latency_Start(&worker_spin[id]);
        while (slot_ready[slot_id] && !shutdown) {
        }
        if (shutdown) {
            return;
        }
        Latency_EndType(&worker_spin[id], 'p');

        Latency_Start(&worker_task[id]);
        Solo solo = prologue_slots[slot_id]();
        prologue_slots[slot_id] = nullptr;
        slot_ready[slot_id] = true;
        Latency_EndType(&worker_task[id], 'p');

        if (solo) {
            Latency_Start(&worker_spin[id]);
            SoloSpin(slot_id);
            if (shutdown) {
                return;
            }
            Latency_EndType(&worker_spin[id], 's');

            Latency_Start(&worker_task[id]);
            solo();
            SoloDone();
            Latency_EndType(&worker_task[id], 's');

            if (epilogue_slots[slot_id]) {
                Latency_Start(&worker_task[id]);
                epilogue_slots[slot_id]();
                epilogue_slots[slot_id] = nullptr;
                Latency_EndType(&worker_task[id], 'e');
            }
        }

        slot_id = (slot_id + n_worker) % n_slot();
    }
}

} // namespace dsnet