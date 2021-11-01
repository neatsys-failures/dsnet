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
        return is_verified;
    }
    // undefiend behavior: call Match() before calling Parse()
    // Serialize() does not enable Match(), because it accepts const `this`, digest cannot be filled
    static bool Match(SignedMessage message1, SignedMessage message2);
private:
    Message &inner_message;
    std::string identifier, digest;
    bool is_verified;
};

}
