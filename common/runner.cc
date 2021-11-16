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

static Latency_t wait_job[128];
static Latency_t wait_solo;
static Latency_t job_total[128];
static Latency_t wait_avail, push_total;

Runner::Runner(int nb_worker_thread) 
    : nb_worker_thread(nb_worker_thread), shutdown(false), job_id(0)
{
    if (NB_CONCURRENT_TASK % nb_worker_thread != 0) {
        Panic("invalid task/thread number pair");
    }
    for (int i = 0; i < nb_worker_thread; i += 1) {
        _Latency_Init(&wait_job[i], "wait_job");
        _Latency_Init(&job_total[i], "job_total");
    }
    _Latency_Init(&wait_solo, "wait_solo");
    _Latency_Init(&wait_avail, "wait_available");
    _Latency_Init(&push_total, "push_total");

    for (int i = 0; i < NB_CONCURRENT_TASK; i += 1) {
        solo_owned[i] = promise<void>();
        available[i] = promise<void>();
        available[i].set_value();
    }

    solo_thread = thread([this]() {
        RunSoloThread();
    });
    epilogue_thread = thread([this]() {
        RunEpilogueThread();
    });
    for (int i = 0; i < nb_worker_thread; i += 1) {
        worker_threads[i] = thread([this, i]() {
            RunWorkerThread(i);
        });
    }

    SetThreadAffinity(pthread_self(), 0);
    SetThreadAffinity(solo_thread.native_handle(), 1);
    SetThreadAffinity(epilogue_thread.native_handle(), 2);
    int cpu = 2;
    for (int i = 0; i < nb_worker_thread; i += 1) {
        cpu += 1;
        while (cpu == 15 || cpu == 32 || cpu == 33 || cpu == 34) {
            cpu += 1;
        }
        if (cpu == 64) {
            break;
        }
        SetThreadAffinity(worker_threads[i].native_handle(), cpu);
    }
}

Runner::~Runner() {
    Latency_t sum_wait_solo, sum_wait_job, sum_job_total;
    _Latency_Init(&sum_wait_job, "wait_job");
    _Latency_Init(&sum_job_total, "job_total");
    for (int i = 0; i < nb_worker_thread; i += 1) {
        Latency_Sum(&sum_wait_job, &wait_job[i]);
        Latency_Sum(&sum_job_total, &job_total[i]);
    }

    Latency_Dump(&wait_solo);
    Latency_Dump(&sum_wait_job);
    Latency_Dump(&sum_job_total);
    Latency_Dump(&wait_avail);
    Latency_Dump(&push_total);

    shutdown = true;
    epilogue_thread.join();
    for (int i = 0; i < nb_worker_thread; i += 1) {
        worker_threads[i].join();
    }
    for (int i = 0; i < NB_CONCURRENT_TASK; i += 1) {
        try {
            solo_owned[i].set_value();
        } catch (future_error) {
            //
        }
    }
    solo_thread.join();
}

void Runner::RunWorkerThread(int worker_id) {
    int job_id = worker_id;
    while (true) {
        Latency_Start(&wait_job[worker_id]);
        Prologue prologue;
        {
            unique_lock<mutex> prologue_lock(prologue_mutexs[job_id]);
            prologue = prologue_jobs[job_id];
            prologue_jobs[job_id] = nullptr;
        }
        Latency_End(&wait_job[worker_id]);

        if (shutdown) {
            return;
        }

        Latency_Start(&job_total[worker_id]);
        char t = 'f';
        unique_lock<mutex> epilogue_lock(epilogue_mutexs[job_id]);
        if (epilogue_jobs[job_id]) {
            Epilogue epilogue = epilogue_jobs[job_id];
            epilogue_jobs[job_id] = nullptr;
            epilogue_lock.unlock();
            epilogue();
        } else {
            epilogue_lock.unlock();
            t = 'p';
        }

        if (prologue) {
            solo_jobs[job_id] = prologue();
            solo_owned[job_id].set_value();
        } else {
            t = t == 'f' ? 'e' : '0';
        }
        job_id = (job_id + nb_worker_thread) % NB_CONCURRENT_TASK;
        Latency_EndType(&job_total[worker_id], t);
    }
}

void Runner::RunSoloThread() {
    for (int job_id = 0;; job_id = (job_id + 1) % NB_CONCURRENT_TASK) {
        solo_job_id = job_id;
        Latency_Start(&wait_solo);
        solo_owned[job_id].get_future().get();
        if (shutdown) {
            Latency_EndType(&wait_solo, 'e');
            return;
        }
        Latency_End(&wait_solo);
        solo_owned[job_id] = promise<void>();

        Solo solo = solo_jobs[job_id];
        solo_jobs[job_id] = nullptr;
        available[job_id].set_value();

        if (solo) {
            solo();
        }
    }
}

void Runner::RunEpilogueThread() {
    for (int job_id = 0;; job_id = (job_id + 1) % NB_CONCURRENT_TASK) {
        if (shutdown) {
            return;
        }

        unique_lock<mutex> epilogue_lock(epilogue_mutexs[job_id]);
        if (epilogue_jobs[job_id]) {
            Epilogue epilogue = epilogue_jobs[job_id];
            epilogue_jobs[job_id] = nullptr;
            epilogue_lock.unlock();
            epilogue();
        } else {
            epilogue_lock.unlock();
        }
    }
}

void Runner::RunPrologue(Prologue prologue) {
    Latency_Start(&push_total);
    Latency_Start(&wait_avail);
    available[job_id].get_future().get();
    Latency_End(&wait_avail);
    available[job_id] = promise<void>();
    {
        unique_lock<mutex> prologue_lock(prologue_mutexs[job_id]);
        prologue_jobs[job_id] = prologue;
    }
    job_id = (job_id + 1) % NB_CONCURRENT_TASK;
    Latency_End(&push_total);
}

void Runner::RunEpilogue(Epilogue epilogue) {
    // if I can assert epilogue job == nullptr here,
    // do I still need to lock?
    unique_lock<mutex> epilogue_lock(epilogue_mutexs[solo_job_id]);
    epilogue_jobs[solo_job_id] = epilogue;
}

}