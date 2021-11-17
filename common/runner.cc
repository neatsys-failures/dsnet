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
static Latency_t solo_push, push_total;

Runner::Runner(int nb_worker_thread) 
    : nb_worker_thread(nb_worker_thread), shutdown(false), last_id(0), solo_id(0)
{
    // if (NB_CONCURRENT_TASK % nb_worker_thread != 0) {
    //     Panic("invalid task/thread number pair");
    // }
    for (int i = 0; i < nb_worker_thread; i += 1) {
        _Latency_Init(&wait_job[i], "wait_job");
        _Latency_Init(&job_total[i], "job_total");
    }
    _Latency_Init(&wait_solo, "wait_solo");
    _Latency_Init(&solo_push, "solo_push");
    _Latency_Init(&push_total, "push_total");

    solo_thread = thread([this]() {
        RunSoloThread();
    });
    for (int i = 0; i < nb_worker_thread; i += 1) {
        worker_threads[i] = thread([this, i]() {
            RunWorkerThread(i);
        });
    }

    SetThreadAffinity(pthread_self(), 0);
    SetThreadAffinity(solo_thread.native_handle(), 1);
    int cpu = 1;
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
    Latency_Dump(&solo_push);
    Latency_Dump(&push_total);

    shutdown = true;
    for (int i = 0; i < nb_worker_thread; i += 1) {
        worker_threads[i].join();
    }
    solo_thread.join();
}

void Runner::RunWorkerThread(int worker_id) {
    while (!shutdown) {
        Latency_Start(&wait_job[worker_id]);
        unique_lock<mutex> worker_lock(worker_task_queue_mutex);
        unique_lock<mutex> tri_lock(tri_mutex);
        if (worker_task_queue.empty()) {
            tri_lock.unlock();
            worker_lock.unlock();
            continue;
        }
        WorkerTask task = worker_task_queue.front();
        worker_task_queue.pop_front();
        tri_lock.unlock();
        worker_lock.unlock();
        Latency_End(&wait_job[worker_id]);

        Assert(task.prologue || task.epilogue);
        Assert(!(task.prologue && task.epilogue));
        if (task.epilogue) {
            task.epilogue();
            continue;
        }

        Solo solo = task.prologue();
        unique_lock<mutex> solo_lock(solo_task_queue_mutex);
        for (auto &solo_task : solo_task_queue) {
            if (solo_task.id == task.id) {
                Assert(!solo_task.ready);
                solo_task.solo = solo;
                solo_task.ready = true;
                solo = nullptr;
                break;
            }
        }
        if (solo) {
            Panic("Worker cannot deliver");
        }
        solo_lock.unlock();
        // Latency_End(&solo_push);
    }
}

void Runner::RunSoloThread() {
    while (!shutdown) {
        unique_lock<mutex> solo_lock(solo_task_queue_mutex);
        if (solo_task_queue.empty()) {
            solo_lock.unlock();
            continue;
        }
        if (!solo_task_queue.front().ready) {
            solo_lock.unlock();
            continue;
        }
        SoloTask task = solo_task_queue.front();
        solo_task_queue.pop_front();
        solo_lock.unlock();

        task.solo();
        Assert(solo_id < task.id);
        solo_id = task.id;
    }
}

void Runner::RunPrologue(Prologue prologue) {
    Latency_Start(&push_total);
    last_id += 1;
    while (last_id > solo_id + nb_worker_thread * 2) {}

    SoloTask solo_task;
    solo_task.id = last_id;
    solo_task.ready = false;
    unique_lock<mutex> solo_lock(solo_task_queue_mutex);
    solo_task_queue.push_back(solo_task);
    solo_lock.unlock();

    WorkerTask task;
    task.id = last_id;
    task.prologue = prologue;
    task.epilogue = nullptr;
    unique_lock<mutex> tri_lock(tri_mutex);
    worker_task_queue.push_back(task);
    tri_lock.unlock();
    Latency_End(&push_total);
}

void Runner::RunEpilogue(Epilogue epilogue) {
    Latency_Start(&solo_push);
    WorkerTask task;
    task.id = 0;
    task.prologue = nullptr;
    task.epilogue = epilogue;
    unique_lock<mutex> worker_lock(tri_mutex);
    worker_task_queue.push_back(task);
    worker_lock.unlock();
    Latency_End(&solo_push);
}

}