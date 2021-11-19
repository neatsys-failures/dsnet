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
    // two special identifier:
    // * "Steve" uses a default test key for signing/verifing. Used in cross-replica communication in bench.
    // * "Alex" no sign/verify at all, omit signature field in message. Used in replica-client communication inn bench.
    SignedAdapter(Message &inner_message, std::string identifier);

    std::string Identifier() const {
        return identifier;
    }
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

    void ParseNoVerify(const void *buf, size_t size);
    void SerializeNoSign(void *buf) const;
};

}
