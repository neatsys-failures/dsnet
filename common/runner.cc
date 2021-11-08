#include "common/runner.h"
#include "lib/assert.h"
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
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63
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

Runner::~Runner() {
    // TODO dump latency
}

void Runner::RunPrologue(Prologue prologue) {
    // next_op is not synchronized currently, because RunPrologue is always
    // invoked from the same thread
    task_slot[next_op].get_future().get();
    task_slot[next_op] = promise<void>();
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
        task_slot[next_op].set_value();
        Solo solo = solo_future.get();
        if (solo) {
            solo();
        }
    });
    next_op += 1;
    if (next_op == NB_CONCURRENT_TASK) {
        next_op = 0;
    }
}

void Runner::RunEpilogue(Epilogue epilogue) {
    worker_pool.push([epilogue](int id) {
        epilogue();
    });
}

}