#include "common/taskqueue.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace std;
using namespace dsnet;
using namespace std::chrono_literals;
using OM = PrologueQueue::OwnedMessage;

TEST(TaskQueue, EmptyDequeue) {
    PrologueQueue q(8);
    ASSERT_EQ(q.Dequeue(), nullptr);
}

class OrderedMessage : public Message {
public:
    int id;
    OrderedMessage(int id) : id(id) {
    }
    Message *Clone() const override {
        return new OrderedMessage(id);
    }
    std::string Type() const override {
        return "NullMessage";
    }
    size_t SerializedSize() const override {
        return 0;
    }
    void Parse(const void *buf, size_t size) override {
        // TODO
        id = 0;
    }
    void Serialize(void *buf) const override {
        // TODO
    }
};

TEST(TaskQueue, OneTask) {
    PrologueQueue q(8);
    q.Enqueue(OM(new OrderedMessage(0)), [](OM message) {
        return message;
    });
    this_thread::sleep_for(10ms);
    ASSERT_NE(q.Dequeue(), nullptr);
    ASSERT_EQ(q.Dequeue(), nullptr);
}

TEST(TaskQueue, Ordered) {
    PrologueQueue q(16);
    for (int i = 0; i < 100; i += 1) {
        q.Enqueue(OM(new OrderedMessage(i)), [i](OM message) {
            this_thread::sleep_for(chrono::milliseconds(100 - i));
            return message;
        });
    }
    this_thread::sleep_for(100ms);
    for (int i = 0; i < 100; i += 1) {
        auto message = q.Dequeue();
        if (message == nullptr) {
            this_thread::sleep_for(100ms);
            message = q.Dequeue();
        }
        ASSERT_NE(message, nullptr);
        ASSERT_EQ(dynamic_cast<OrderedMessage *>(&*message)->id, i);
    }
}
