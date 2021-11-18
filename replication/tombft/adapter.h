#pragma once
#include "lib/transport.h"

namespace dsnet {
namespace tombft {

class TOMBFTAdapter: public Message {
public:
    // `multicast` is only considered during serialization
    // parsing determine multicast by message content
    TOMBFTAdapter(Message &inner, const std::string &identifier, bool multicast);

    // override methods
    TOMBFTAdapter *Clone() const override;
    std::string Type() const override {
        return "TOMBFT[" + inner.Type() + "]";
    }
    size_t SerializedSize() const override;
    void Parse(const void *buf, size_t size) override;
    void Serialize(void *buf) const override;

    // receiver-side methods
    // call these methods without/before call `Parse` has undefined behavior
    bool IsVerified() const {
        return is_verified;
    }
    bool IsMulticast() const {
        return is_multicast;
    }
    uint32_t MessageNumber() const {
        return message_number;
    }
    uint8_t SessionNumber() const {
        return session_number;
    }
private:
    Message &inner;
    std::string identifier;
    bool is_multicast, is_verified;
    uint32_t message_number;
    uint8_t session_number;
};

}
}