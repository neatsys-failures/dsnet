#pragma once
#include "lib/transport.h"

namespace dsnet {

// Message with trusted content made by node with certain identifier.
// Every node should get an identifier from (imagined) identity providing
// service. The identifier should be fixed, binding to address for replicas or
// client id for clients. Internal to SignedAdapter, each identifier is
// associated with a secret key, which should be configured upon system start up
// (WIP).
class SignedAdapter : public Message {
public:
    Message *Clone() const override;
    std::string Type() const override;
    size_t SerializedSize() const override;
    void Parse(const void *buf, size_t size) override;
    void Serialize(void *buf) const override;
    // two special identifier:
    // * "Steve" uses a default test key for signing/verifing. Used in
    // cross-replica communication in bench.
    // * "Alex" a special identifier that do not include signatures in messages,
    // and always be trusted. Replica and client specify this identifier when
    // send to each other to eliminate signature overhead
    // sign = false will override identifier with "Alex", which make the code
    // looks more reasonable
    // identifier and sign are ignored during Parse()
    SignedAdapter(
        Message &inner_message, std::string identifier, bool sign = true);

    // receiver-side accessors
    // calling these before/without calling Parse() has undefined behavior
    std::string Identifier() const { return identifier; }
    bool IsVerified() const { return is_verified; }
    const std::string &Signature() const { return signature; }

    // auxiliry method, perform verification on in-memory parsed (or more
    // accurate, artifact) packet currently mainly for weird logic in HotStuff
    // (in combination of weird API of Message/Adapter)
    // The following:
    //   SignedAdapter signed_layer(inner, id);
    //   assert(signed_layer.DoVerify(signature));
    // is equal to:
    //   inner.Serialize(buf + offset);
    //   <put id and signature to proper location in buf to compose a signed
    //   layout>
    //   SignedAdapter signed_layer(inner, id);
    //   signed_layer.Parse(buf, size);
    //   assert(signed_layer.IsVerified());
    bool DoVerify(const std::string &signature) const;

private:
    Message &inner_message;
    std::string identifier, signature;
    bool is_verified;

    void ParseNoVerify(const void *buf, size_t size);
    void SerializeNoSign(void *buf) const;
};

} // namespace dsnet
