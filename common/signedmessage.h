#pragma once
#include "lib/transport.h"

namespace dsnet {

class SignedMessage : public Message {
public:
    Message *Clone() const override;
    std::string Type() const override;
    size_t SerializedSize() const override;
    void Parse(const void *buf, size_t size) override;
    void Serialize(void *buf) const override;
    // Sender should provide self identifier, receiver should determine remote identifier by some way
    // e.g., by remote address, then provide it before parsing
    // two special identifier:
    // * "Steve" uses a default test key for signing/verifing. Used by benchmark clients/replicas.
    // * "Alex" to be decided
    SignedMessage(Message &inner_message, std::string identifier);
    // undefined behavior: call IsVerified() before/without calling Parse()
    bool IsVerified() const {
        return is_verified;
    }
    // undefined behavior: call Digest() before/without calling Parse()
    const std::string &Digest() const {
        return digest;
    }
private:
    Message &inner_message;
    std::string identifier, digest;
    bool is_verified;
};

}
