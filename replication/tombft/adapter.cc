#include "replication/tombft/adapter.h"

#include <cstring>
#include <endian.h>

namespace dsnet {
namespace tombft {

using std::memset;

struct __attribute__((packed)) Layout {
    union {
        uint8_t digest[32];
        struct {
            uint8_t _unused1;
            uint8_t session_number;
            uint8_t _unused2;
            uint8_t shard_number;
            uint32_t message_number;
            uint8_t signature[64];
            uint8_t inner_buf[0];
        } multicast;
        struct {
            uint8_t _unused1;
            uint8_t flag;
            uint8_t inner_buf[0];
        } unicast;
    };
};

TOMBFTAdapter::TOMBFTAdapter(Message &inner, const string &identifier, bool multicast)
    : inner(inner), identifier(identifier), is_multicast(multicast)
{
}

TOMBFTAdapter *TOMBFTAdapter::Clone() const {
    TOMBFTAdapter *adapter = new TOMBFTAdapter(inner, identifier, is_multicast);
    adapter->is_verified = is_verified;
    adapter->message_number = message_number;
    adapter->session_number = session_number;
    return adapter;
}

size_t TOMBFTAdapter::SerializedSize() const {
    if (is_multicast) {
        return sizeof(Layout().multicast) + inner.SerializedSize();
    } else {
        return sizeof(Layout().unicast) + inner.SerializedSize();
    }
}

void TOMBFTAdapter::Serialize(void *buf) const {
    Layout *layout = (Layout *) buf;
    if (!is_multicast) {
        memset(layout, 0, sizeof(layout->unicast));
        inner.Serialize(layout->unicast.inner_buf);
        return;
    }

    inner.Serialize(layout->multicast.inner_buf);
    // TODO digest
    layout->multicast._unused1 = 0;
    layout->multicast._unused2 = 0;
    layout->multicast.session_number = 0;
    layout->multicast.shard_number = 0;
    layout->multicast.message_number = 0;
}

void TOMBFTAdapter::Parse(const void *buf, size_t size) {
    const Layout *layout = (const Layout *) buf;
    is_multicast = layout->unicast.flag;
    if (!is_multicast) {
        is_verified = true;
        message_number = 0;
        session_number = 0;
        inner.Parse(layout->unicast.inner_buf, size - sizeof(layout->unicast));
        return;
    }

    // TODO verify
    is_verified = true;
    session_number = layout->multicast.session_number;
    message_number = be32toh(layout->multicast.message_number);
    inner.Parse(layout->multicast.inner_buf, size - sizeof(layout->multicast));
}

}
}