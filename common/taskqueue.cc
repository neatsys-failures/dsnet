#include "common/taskqueue.h"

#include <chrono>

using std::future_status;
using std::move;
using std::chrono_literals::operator""s;
using std::unique_ptr;

namespace dsnet {

PrologueQueue::PrologueQueue(int nb_thread)
    : pool(nb_thread)
{
}

void PrologueQueue::Enqueue(unique_ptr<AbstractPrologueTask> task, Prologue prologue)
{
    WorkingTask working;
    working.data = task.release();
    working.handle = pool.push([] (int id, Prologue prologue, AbstractPrologueTask *task) {
        return prologue(*task);
    }, prologue, working.data);
    tasks.push(move(working));
}

auto PrologueQueue::Dequeue() -> unique_ptr<AbstractPrologueTask> {
    while (tasks.size() != 0) {
        auto status = tasks.front().handle.wait_for(0s);
        if (status != future_status::ready) {
            return nullptr;
        }
        WorkingTask task = move(tasks.front());
        tasks.pop();
        if (task.data->HasMessage()) {
            return unique_ptr<AbstractPrologueTask>(task.data);
        } else {
            delete task.data;
        }
    }
    return nullptr;
}

}
