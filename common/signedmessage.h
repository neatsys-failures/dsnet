#pragma once
#include "lib/transport.h"

namespace dsnet {

class SignedMessage : public Message {
    Message *Clone() const override;
    std::string Type() const override;
    size_t SerializedSize() const override;
    void Parse(const void *buf, size_t size) override;
    void Serialize(void *buf) const override;
public:
    // two special identifier:
    // * "Steve" uses a default test key for signing/verifing. Used by benchmark clients/replicas.
    // * "Alex" to be decided
    SignedMessage(Message &inner_message, std::string identifier);
    // undefined behavior: call IsVerified() before/without calling Parse()
    bool IsVerified() const {
        return verified;
    }
private:
    Message &inner_message;
    bool verified;
};

}
