#include "replication/tombft/adapter.h"

#include <cstring>
#include <endian.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include "lib/assert.h"

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

static const unsigned char SWITCH_SECKEY[] = {
    0x73, 0x77, 0x74, 0x69, 0x63, 0x68, 0x00, 0x00,  // print "switch" as C string
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
static __thread const secp256k1_pubkey *SWITCH_PUBKEY;
static __thread secp256k1_context *PROTO_CTX_VERIFY = nullptr;
static __thread secp256k1_ecdsa_signature REPLACED_SIG;

TOMBFTAdapter::TOMBFTAdapter(Message &inner, bool multicast)
    : inner(inner), is_multicast(multicast)
{
    if (PROTO_CTX_VERIFY != nullptr) {
        return;
    }
    PROTO_CTX_VERIFY = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    SWITCH_PUBKEY = new secp256k1_pubkey;
    secp256k1_context *sign_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    int code = secp256k1_ec_pubkey_create(sign_ctx, (secp256k1_pubkey *) SWITCH_PUBKEY, SWITCH_SECKEY);
    ASSERT(code);
    code = secp256k1_ecdsa_sign(sign_ctx, &REPLACED_SIG, SWITCH_SECKEY, SWITCH_SECKEY, nullptr, nullptr);
    ASSERT(code);
    secp256k1_context_destroy(sign_ctx);
}

TOMBFTAdapter *TOMBFTAdapter::Clone() const {
    TOMBFTAdapter *adapter = new TOMBFTAdapter(inner, is_multicast);
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
    SHA256(layout->multicast.inner_buf, inner.SerializedSize(), layout->digest);
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

    unsigned char digest[32];
    SHA256(layout->multicast.inner_buf, size - sizeof(layout->multicast), digest);
    secp256k1_context *ctx = PROTO_CTX_VERIFY;
    secp256k1_ecdsa_signature sig, *sig_p = &sig;
    if (!secp256k1_ecdsa_signature_parse_compact(ctx, &sig, layout->multicast.signature)) {
#ifdef DSNET_TOM_FPGA_DEMO
        sig_p = &REPLACED_SIG;
#else
        is_verified = false;
        return;
#endif
    }

#ifdef DSNET_TOM_FPGA_DEMO
    is_verified = secp256k1_ecdsa_verify(ctx, sig_p, digest, SWITCH_PUBKEY) || layout->multicast.signature[0] >= 0x30;
#else
    is_verified = secp256k1_ecdsa_verify(ctx, sig_p, digest, SWITCH_PUBKEY);
#endif
    if (!is_verified) {
        return;
    }
    session_number = layout->multicast.session_number;
    message_number = be32toh(layout->multicast.message_number);
    if (message_number == 0) {
        Panic("TOM message not sequenced");
    }
    inner.Parse(layout->multicast.inner_buf, size - sizeof(layout->multicast));
}

}
}