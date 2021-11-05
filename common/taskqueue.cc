#include "common/taskqueue.h"
#include "lib/latency.h"
#include "lib/message.h"

#include <chrono>
#include <sstream>

using std::future_status;
using std::move;
using std::chrono_literals::operator""s;
using std::unique_ptr;
using std::stringstream;

namespace dsnet {

static Latency_t prologue_latency[256];
static Latency_t prologue_latency_sum;
static Latency_t enqueue_latency, dequeue_latency;

#ifndef DSNET_SIMPLE_TASKQUEUE

PrologueQueue::PrologueQueue(int nb_thread)
    : pool(nb_thread), nb_thread(nb_thread)
{
    _Latency_Init(&enqueue_latency, "enqueue");
    _Latency_Init(&dequeue_latency, "dequeue");
    for (int i = 0; i < nb_thread; i += 1) {
        stringstream ss;
        ss << "prologue_task#" << i;
        _Latency_Init(&prologue_latency[i], ss.str().c_str());
    }
}

PrologueQueue::~PrologueQueue() {
    _Latency_Init(&prologue_latency_sum, "prologue_task");
    for (int i = 0; i < nb_thread; i += 1) {
        Latency_Sum(&prologue_latency_sum, &prologue_latency[i]);
    }
    Latency_Dump(&enqueue_latency);
    Latency_Dump(&prologue_latency_sum);
    Latency_Dump(&dequeue_latency);
}

void PrologueQueue::Enqueue(unique_ptr<PrologueTask> task, Prologue prologue)
{
    Latency_Start(&enqueue_latency);
    WorkingTask working;
    working.data = task.release();
    working.handle = pool.push([] (int id, Prologue prologue, PrologueTask *task) {
        Latency_Start(&prologue_latency[id]);
        prologue(*task);
        Latency_End(&prologue_latency[id]);
    }, prologue, working.data);
    tasks.push(move(working));
    Latency_End(&enqueue_latency);
}

auto PrologueQueue::Dequeue() -> unique_ptr<PrologueTask> {
    Latency_Start(&dequeue_latency);
    while (tasks.size() != 0) {
        auto status = tasks.front().handle.wait_for(0s);
        if (status != future_status::ready) {
            Latency_EndType(&dequeue_latency, 'w');
            return nullptr;
        }
        WorkingTask task = move(tasks.front());
        tasks.pop();
        if (task.data->HasMessage()) {
            Latency_End(&dequeue_latency);
            return unique_ptr<PrologueTask>(task.data);
        } else {
            delete task.data;
        }
    }
    Latency_EndType(&dequeue_latency, 'e');
    return nullptr;
}

#else

PrologueQueue::PrologueQueue(int nb_thread) {
    Notice("Simple PrologueQueue enabled");
    _Latency_Init(&enqueue_latency, "enqueue");
    _Latency_Init(&dequeue_latency, "dequeue");
    _Latency_Init(&prologue_latency_sum, "prologue_task");
}

PrologueQueue::~PrologueQueue() {
    Latency_Dump(&enqueue_latency);
    Latency_Dump(&prologue_latency_sum);
    Latency_Dump(&dequeue_latency);
}

void PrologueQueue::Enqueue(unique_ptr<PrologueTask> task, Prologue prologue) {
    Latency_Start(&prologue_latency_sum);
    prologue(*task);
    Latency_End(&prologue_latency_sum);
    Latency_Start(&enqueue_latency);
    simple_tasks.push(move(task));
    Latency_End(&enqueue_latency);
}

auto PrologueQueue::Dequeue() -> unique_ptr<PrologueTask> {
    Latency_Start(&dequeue_latency);
    while (simple_tasks.size() != 0) {
        unique_ptr<PrologueTask> task = move(simple_tasks.front());
        simple_tasks.pop();
        if (task->HasMessage()) {
            Latency_End(&dequeue_latency);
            return move(task);
        }
    }
    Latency_EndType(&dequeue_latency, 'e');
    return nullptr;
}

#endif

}
