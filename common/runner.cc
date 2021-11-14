#include "common/runner.h"
#include "lib/assert.h"
#include "lib/latency.h"
#include <future>
#include <pthread.h>

namespace dsnet {

static void SetThreadAffinity(pthread_t thread, int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    int status = pthread_setaffinity_np(thread, sizeof(mask), &mask);
    ASSERT(status == 0);
}

using Solo = std::function<void ()>;
using std::promise;
using std::move;
using std::future;

const int WORKER_CPU[] = {
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    15, 32, 33
};

Runner::Runner(int nb_worker_thread) 
    : worker_pool(nb_worker_thread), solo_thread(1), next_op(0)
{
    SetThreadAffinity(pthread_self(), 0);
    SetThreadAffinity(solo_thread.get_thread(0).native_handle(), 1);
    for (int i = 0; i < nb_worker_thread; i += 1) {
        if (i == 62) {
            break;
        }
        SetThreadAffinity(worker_pool.get_thread(i).native_handle(), WORKER_CPU[i + 2]);
    }

    for (int i = 0; i < NB_CONCURRENT_TASK; i += 1) {
        task_slot[i].set_value();
    }
}

DEFINE_LATENCY(solo_wait);
DEFINE_LATENCY(solo_work);
DEFINE_LATENCY(prologue_wait);
DEFINE_LATENCY(push);

Runner::~Runner() {
    Latency_Dump(&prologue_wait);
    Latency_Dump(&solo_wait);
    Latency_Dump(&solo_work);
    Latency_Dump(&push);
}

void Runner::RunPrologue(Prologue prologue) {
    // next_op is not synchronized currently, because RunPrologue is always
    // invoked from the same thread
    Latency_Start(&prologue_wait);
    // task_slot[next_op].get_future().get();
    Latency_End(&prologue_wait);
    Latency_Start(&push);
    // task_slot[next_op] = promise<void>();
    auto solo_future = worker_pool.push([
        prologue
    ](int id) mutable {
        Solo solo = prologue();
        return solo;
    });
    solo_thread.push([
        this,
        next_op = this->next_op,
        solo_future = move(solo_future)
    ](int id) mutable {
        Latency_Start(&solo_wait);
        Solo solo = solo_future.get();
        Latency_End(&solo_wait);
        // task_slot[next_op].set_value();
        Latency_Start(&solo_work);
        if (solo) {
            solo();
        }
        Latency_End(&solo_work);
    });
    next_op += 1;
    if (next_op == NB_CONCURRENT_TASK) {
        next_op = 0;
    }
    Latency_End(&push);
}

void Runner::RunEpilogue(Epilogue epilogue) {
    worker_pool.push([epilogue](int id) {
        epilogue();
    });
}

}