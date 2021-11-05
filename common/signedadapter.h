#pragma once
#include "lib/transport.h"

namespace dsnet {

// Message with trusted content made by node with certain identifier.
// Every node should get an identifier from (imagined) identity providing service.
// The identifier should be fixed, binding to address for replicas or client id for clients.
// Internal to SignedAdapter, each identifier is associated with a secret key, 
// which should be configured upon system start up (WIP).
class SignedAdapter : public Message {
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
    SignedAdapter(Message &inner_message, std::string identifier);
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
