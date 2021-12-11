#pragma once
#include "lib/transport.h"

namespace dsnet {
namespace tombft {

class TOMBFTAdapter : public Message {
public:
    // `multicast` is only considered during serialization
    // parsing determine multicast by message content
    TOMBFTAdapter(Message &inner, bool multicast);

    // override methods
    TOMBFTAdapter *Clone() const override;
    std::string Type() const override { return "TOMBFT[" + inner.Type() + "]"; }
    size_t SerializedSize() const override;
    void Parse(const void *buf, size_t size) override;
    void Serialize(void *buf) const override;

    // receiver-side methods
    // call these methods without/before call `Parse` has undefined behavior
    bool IsVerified() const { return is_verified; }
    bool IsMulticast() const { return is_multicast; }
    bool IsSigned() const { return is_signed; }
    uint32_t MessageNumber() const { return message_number; }
    uint8_t SessionNumber() const { return session_number; }

private:
    Message &inner;
    bool is_multicast, is_verified, is_signed;
    uint32_t message_number;
    uint16_t session_number;
};

class TOMBFTHMACAdapter : public Message {
public:
    // sending side: multicast matters, replica id not care
    // receiving side: multicast not care, replica id set to receiver's one
    // replica id is used as identifier in this case, because we do not have a
    // identifier -> replica id lookup spec now
    TOMBFTHMACAdapter(Message &inner, int replica_id, bool multicast)
        : inner(inner), replica_id(replica_id), is_multicast(multicast) {}

    TOMBFTHMACAdapter *Clone() const override {
        TOMBFTHMACAdapter *cloned =
            new TOMBFTHMACAdapter(inner, replica_id, is_multicast);
        cloned->is_verified = is_verified;
        return cloned;
    }
    std::string Type() const override {
        return "TOMBFTHMAC[" + inner.Type() + "]";
    }
    size_t SerializedSize() const override;
    void Parse(const void *buf, size_t size) override;
    void Serialize(void *buf) const override;

    bool IsVerified() const { return is_verified; }
    bool IsMulticast() const { return is_multicast; }
    uint32_t MessageNumber() const { return message_number; }
    uint8_t SessionNumber() const { return session_number; }

private:
    Message &inner;
    int replica_id;
    bool is_multicast, is_verified;
    uint32_t message_number;
    uint16_t session_number;
};

} // namespace tombft
} // namespace dsnet