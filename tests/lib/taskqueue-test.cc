#include "common/taskqueue.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace std;
using namespace dsnet;
using namespace std::chrono_literals;

TEST(TaskQueue, EmptyDequeue) {
    PrologueQueue q(8);
    ASSERT_EQ(q.Dequeue(), nullptr);
}

class NullAddress : public TransportAddress {
    TransportAddress *clone() const override {
        return new NullAddress();
    }
};

using OT = std::unique_ptr<PrologueTask<int>>;  // owned task
TEST(TaskQueue, OneTask) {
    PrologueQueue q(8);
    NullAddress addr;
    q.Enqueue(OT(new PrologueTask<int>(nullptr, 0, addr)), [](AbstractPrologueTask &task) {
        task.SetMessage(new int(0));
    });
    this_thread::sleep_for(10ms);
    ASSERT_NE(q.Dequeue(), nullptr);
    ASSERT_EQ(q.Dequeue(), nullptr);
}

TEST(TaskQueue, Ordered) {
    PrologueQueue q(16);
    NullAddress addr;
    for (int i = 0; i < 100; i += 1) {
        q.Enqueue(OT(new PrologueTask<int>(nullptr, 0, addr)), [i](AbstractPrologueTask &task) {
            this_thread::sleep_for(chrono::milliseconds(100 - i));
            task.SetMessage(new int(i));
        });
    }
    this_thread::sleep_for(100ms);
    for (int i = 0; i < 100; i += 1) {
        auto task = q.Dequeue();
        if (task == nullptr) {
            this_thread::sleep_for(100ms);
            task = q.Dequeue();
        }
        ASSERT_NE(task, nullptr);
        ASSERT_EQ(task->Message<int>(), i);
    }
}
