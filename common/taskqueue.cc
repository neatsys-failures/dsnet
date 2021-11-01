#include "common/taskqueue.h"

#include <chrono>

using std::future_status;
using std::move;
using std::chrono_literals::operator""s;

namespace dsnet {

PrologueQueue::PrologueQueue(int nb_thread)
    : pool(nb_thread)
{
}

void PrologueQueue::Enqueue(OwnedMessage message, Prologue prologue)
{
    // CTPL does not support unique_ptr as argument
    tasks.push(pool.push([prologue] (int id, Message *message) {
        return prologue(OwnedMessage(message));
    }, message.release()));
}

auto PrologueQueue::Dequeue() -> OwnedMessage {
    while (tasks.size() != 0) {
        auto status = tasks.front().wait_for(0s);
        if (status != future_status::ready) {
            return nullptr;
        }
        OwnedMessage result = tasks.front().get();
        tasks.pop();
        if (result != nullptr) {
            return result;
        }
    }
    return nullptr;
}

}
