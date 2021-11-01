#pragma once
#include "lib/transport.h"

#include <functional>
#include <queue>
#include <memory>

namespace dsnet {

// General parallization for message-preprocessing.

class PrologueQueue {
    using Prologue = std::function<bool (std::unique_ptr<Message>)>;
public:
    PrologueQueue(const Prolugue prologue);
private:
    const Prolugue prologue;

    std::queue<Task> tasks;
};

}