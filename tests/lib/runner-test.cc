#include "common/runner.h"
#include <gtest/gtest.h>
#include <chrono>

TEST(Runner, Null) {
    //
}

using dsnet::Runner;

TEST(Runner, Instance) {
    Runner runner(8);
    ASSERT_NE(&runner, nullptr);
}

TEST(Runner, OnePrologue) {
    Runner runner(8);
    runner.RunPrologue([]() {
        return nullptr;
    });
}

TEST(Runner, OneSolo) {
    Runner runner(8);
    runner.RunPrologue([]() {
        return []() {
        };
    });
}

using std::chrono::milliseconds;
using std::this_thread::sleep_for;

TEST(Runner, SoloOrder) {
    Runner runner(8);
    int last_solo = 0;
    for (int i = 0; i < 10; i += 1) {
        runner.RunPrologue([i, &last_solo]() {
            sleep_for(milliseconds(10 - i));
            return [i, &last_solo]() {
                ASSERT_EQ(last_solo, i);
                last_solo += 1;
            };
        });
    }
}
