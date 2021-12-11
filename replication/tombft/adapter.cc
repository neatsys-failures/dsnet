#include "replication/tombft/adapter.h"

#include "lib/assert.h"
#include <cstring>
#include <endian.h>
#include <openssl/sha.h>
#include <secp256k1.h>

namespace dsnet {
namespace tombft {

using std::memset;

struct __attribute__((packed)) Layout {
    union {
        uint8_t digest[32];
        struct {
            uint16_t session_number;
            uint8_t _unused1[2];
            uint32_t message_number;
            uint8_t chain_hash[4];
            uint8_t _unused2[4];
            uint8_t signature[64];
            uint8_t inner_buf[0];
        } multicast;
        struct {
            uint16_t flag;
            uint8_t inner_buf[0];
        } unicast;
    };
};

static const unsigned char SWITCH_SECKEY[] = {
    // print "switch" as C string
    0x73, 0x77, 0x74, 0x69, 0x63, 0x68, 0x00, 0x00, //
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, //
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, //
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, //
};
static __thread const secp256k1_pubkey *SWITCH_PUBKEY;
static __thread secp256k1_context *PROTO_CTX_VERIFY = nullptr;
// currently network cannot generate valid signature bytes, only random
// placeholder
// if the placeholder cannot pass deserialization stage, replacing in-memory
// representation with this fallback version, so the verification overhead will
// always be correct
static __thread secp256k1_ecdsa_signature REPLACED_SIG;

TOMBFTAdapter::TOMBFTAdapter(Message &inner, bool multicast)
    : inner(inner), is_multicast(multicast) {
    if (PROTO_CTX_VERIFY != nullptr) {
        return;
    }
    PROTO_CTX_VERIFY = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    SWITCH_PUBKEY = new secp256k1_pubkey;
    secp256k1_context *sign_ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    int code = secp256k1_ec_pubkey_create(
        sign_ctx, (secp256k1_pubkey *)SWITCH_PUBKEY, SWITCH_SECKEY);
    ASSERT(code);
    code = secp256k1_ecdsa_sign(
        sign_ctx, &REPLACED_SIG, SWITCH_SECKEY, SWITCH_SECKEY, nullptr,
        nullptr);
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
    Layout *layout = (Layout *)buf;
    if (!is_multicast) {
        memset(layout, 0, sizeof(layout->unicast));
        inner.Serialize(layout->unicast.inner_buf);
        return;
    }

    inner.Serialize(layout->multicast.inner_buf);
    SHA256(layout->multicast.inner_buf, inner.SerializedSize(), layout->digest);
    layout->multicast.session_number = 0;
    layout->multicast.message_number = 0;
    memset(
        layout->multicast.chain_hash, 0, sizeof(layout->multicast.chain_hash));
    memset(layout->multicast.signature, 0, sizeof(layout->multicast.signature));
}

void TOMBFTAdapter::Parse(const void *buf, size_t size) {
    const Layout *layout = (const Layout *)buf;
    is_multicast = layout->unicast.flag;
    if (!is_multicast) {
        is_verified = true;
        message_number = 0;
        session_number = 0;
        inner.Parse(layout->unicast.inner_buf, size - sizeof(layout->unicast));
        return;
    }

    is_signed = false;
    for (unsigned i = 0; i < sizeof(layout->multicast.signature); i += 1) {
        if (layout->multicast.signature[i] != 0) {
            is_signed = true;
            break;
        }
    }
    if (!is_signed) {
        is_verified = true;
    } else {
        // restore the "correct" digest in switch's view
        Layout regen;
        SHA256(
            layout->multicast.inner_buf, size - sizeof(regen.multicast),
            regen.digest);
        regen.multicast.session_number = layout->multicast.session_number;
        regen.multicast.message_number = layout->multicast.message_number;
        memcpy(
            regen.multicast.chain_hash, layout->multicast.chain_hash,
            sizeof(layout->multicast.chain_hash));
        memset(regen.multicast.signature, 0, sizeof(regen.multicast.signature));

        secp256k1_context *ctx = PROTO_CTX_VERIFY;
        secp256k1_ecdsa_signature sig, *sig_p = &sig;
        if (!secp256k1_ecdsa_signature_parse_compact(
                ctx, sig_p, layout->multicast.signature)) {
#ifdef DSNET_TOM_FPGA_DEMO
            sig_p = &REPLACED_SIG;
#else
            is_verified = false;
            return;
#endif
        }

#ifdef DSNET_TOM_FPGA_DEMO
        is_verified =
            secp256k1_ecdsa_verify(ctx, sig_p, regen.digest, SWITCH_PUBKEY) ||
            // layout->multicast.signature[0] >= 0x30;
            layout->multicast.message_number;
#else
        is_verified =
            secp256k1_ecdsa_verify(ctx, sig_p, regen.digest, SWITCH_PUBKEY);
#endif
    }

    if (!is_verified) {
        return;
    }
    message_number = be32toh(layout->multicast.message_number);
    session_number = be16toh(layout->multicast.session_number);
    if (message_number == 0) {
        Panic("TOM message not sequenced");
    }
    inner.Parse(layout->multicast.inner_buf, size - sizeof(layout->multicast));
}

struct __attribute__((packed)) HMACLayout {
    union {
        uint8_t digest[32];
        struct {
            uint16_t session_number;
            uint8_t _unused1[2];
            uint32_t message_number;
            uint8_t _reserved[4];
            uint8_t _unused2[4];
            uint8_t hmac[4][4];
            uint8_t inner_buf[0];
        } multicast;
        struct {
            uint16_t flag;
            uint8_t inner_buf[0];
        } unicast;
    };
};

size_t TOMBFTHMACAdapter::SerializedSize() const {
    if (is_multicast) {
        return sizeof(HMACLayout().multicast) + inner.SerializedSize();
    } else {
        return sizeof(HMACLayout().unicast) + inner.SerializedSize();
    }
}

void TOMBFTHMACAdapter::Serialize(void *buf) const {
    HMACLayout *layout = (HMACLayout *)buf;
    if (!is_multicast) {
        memset(layout, 0, sizeof(layout->unicast));
        inner.Serialize(layout->unicast.inner_buf);
        return;
    }

    inner.Serialize(layout->multicast.inner_buf);
    SHA256(layout->multicast.inner_buf, inner.SerializedSize(), layout->digest);
    layout->multicast.session_number = 0;
    layout->multicast.message_number = 0;
    memset(layout->multicast._reserved, 0, sizeof(layout->multicast._reserved));
    memset(layout->multicast.hmac, 0, sizeof(layout->multicast.hmac));
}

void TOMBFTHMACAdapter::Parse(const void *buf, size_t size) {
    const HMACLayout *layout = (const HMACLayout *)buf;
    is_multicast = layout->unicast.flag;
    if (!is_multicast) {
        is_verified = true;
        message_number = 0;
        session_number = 0;
        inner.Parse(layout->unicast.inner_buf, size - sizeof(layout->unicast));
        return;
    }
    // restore the "correct" digest in switch's view
    HMACLayout regen;
    SHA256(
        layout->multicast.inner_buf, size - sizeof(regen.multicast),
        regen.digest);
    regen.multicast.session_number = layout->multicast.session_number;
    regen.multicast.message_number = layout->multicast.message_number;
    memset(regen.multicast._reserved, 0, sizeof(regen.multicast._reserved));
    memset(regen.multicast.hmac, 0, sizeof(regen.multicast.hmac));

    // TODO halfsiphash here
    is_verified = true;

    if (!is_verified) {
        return;
    }
    message_number = be32toh(layout->multicast.message_number);
    session_number = be16toh(layout->multicast.session_number);
    if (message_number == 0) {
        Panic("TOM message not sequenced");
    }
    inner.Parse(layout->multicast.inner_buf, size - sizeof(layout->multicast));
}

} // namespace tombft
} // namespace dsnet