#include "common/taskqueue.h"
#include <gtest/gtest.h>

using namespace dsnet;

TEST(TaskQueue, EmptyDequeue) {
    PrologueQueue q(8);
    ASSERT_EQ(q.Dequeue(), nullptr);
}
