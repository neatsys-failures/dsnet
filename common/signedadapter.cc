#include "common/signedadapter.h"
#include "lib/assert.h"
#include "lib/message.h"

#include <secp256k1.h>
#include <openssl/sha.h>
#include <cstring>

using std::string;
using std::strcmp;
using std::memset;
using std::strcpy;

namespace dsnet {

static const unsigned char STEVE_SECKEY[] = {
    0x53, 0x74, 0x65, 0x76, 0x65, 0x00, 0x00, 0x00,  // print "Steve" as C string
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
static __thread const secp256k1_pubkey *STEVE_PUBKEY;
static __thread secp256k1_context *PROTO_CTX_SIGN = nullptr, *PROTO_CTX_VERIFY = nullptr;

SignedAdapter::SignedAdapter(Message &inner_message, const string identifier) 
    : inner_message(inner_message), identifier(identifier) 
{
    if (PROTO_CTX_SIGN != nullptr) {
        return;
    }
    PROTO_CTX_SIGN = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    PROTO_CTX_VERIFY = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    STEVE_PUBKEY = new secp256k1_pubkey;
    int code = secp256k1_ec_pubkey_create(PROTO_CTX_SIGN, (secp256k1_pubkey *) STEVE_PUBKEY, STEVE_SECKEY);
    ASSERT(code);
}

Message *SignedAdapter::Clone() const {
    SignedAdapter *message = new SignedAdapter(inner_message, identifier);
    message->is_verified = is_verified;
    message->digest = digest;
    return message;
}

string SignedAdapter::Type() const {
    return inner_message.Type();
}

size_t SignedAdapter::SerializedSize() const {
    // sign overhead: 64B signature + 16B identifier
    return inner_message.SerializedSize() + 80;
}

#ifndef DSNET_NO_SIGN

void SignedAdapter::Parse(const void *buf, size_t size) {
    const char *buf_id = (const char *) buf;
    const unsigned char *buf_sig = ((const unsigned char *) buf) + 16;
    const unsigned char *buf_inner = ((const unsigned char *) buf) + 80;
    size_t inner_size = size - 80;

    if (buf_id[15] != '\0') {
        is_verified = false;
        return;
    }
    identifier.assign(buf_id);
    const secp256k1_pubkey *pubkey;
    if (identifier == "Steve") {
        pubkey = STEVE_PUBKEY;
    } else {
        NOT_IMPLEMENTED();
    }
    secp256k1_context *ctx = PROTO_CTX_VERIFY;
    secp256k1_ecdsa_signature sig;
    unsigned char *digest;  // must be declared before goto
    if (!secp256k1_ecdsa_signature_parse_compact(ctx, &sig, buf_sig)) {
        is_verified = false;
        return;
    }
    digest = SHA256(buf_inner, inner_size, new unsigned char[32]);
    this->digest.assign((char *)digest, 32);
    is_verified = secp256k1_ecdsa_verify(ctx, &sig, digest, pubkey) == 1;
    delete[] digest;
    if (is_verified) {
        inner_message.Parse(buf_inner, inner_size);
    }
}

void SignedAdapter::Serialize(void *buf) const {
    char *buf_id = (char *)buf;
    unsigned char *buf_sig = ((unsigned char *) buf) + 16;
    unsigned char *buf_inner = ((unsigned char *) buf) + 80;
    inner_message.Serialize(buf_inner);
    size_t inner_size = inner_message.SerializedSize();

    memset(buf_id, 0, 16);
    ASSERT(identifier.size() < 16);
    identifier.copy(buf_id, identifier.size());
    const unsigned char *seckey;
    if (identifier == "Steve") {
        seckey = STEVE_SECKEY;
    } else {
        NOT_IMPLEMENTED();
    }
    secp256k1_context *ctx = PROTO_CTX_SIGN;
    unsigned char *digest = SHA256(buf_inner, inner_size, new unsigned char[32]);
    secp256k1_ecdsa_signature sig;
    int code = secp256k1_ecdsa_sign(ctx, &sig, digest, seckey, nullptr, nullptr);
    ASSERT(code);
    delete[] digest;
    code = secp256k1_ecdsa_signature_serialize_compact(ctx, buf_sig, &sig);
    ASSERT(code);
}

#else

void SignedAdapter::Parse(const void *buf, size_t size) {
    const unsigned char *buf_inner = ((const unsigned char *) buf) + 80;
    size_t inner_size = size - 80;

    unsigned char *digest = SHA256(buf_inner, inner_size, new unsigned char[32]);
    this->digest.assign((char *)digest, 32);
    is_verified = true;
    delete[] digest;
    inner_message.Parse(buf_inner, inner_size);
}

void SignedAdapter::Serialize(void *buf) const {
    char *buf_id = (char *)buf;
    unsigned char *buf_inner = ((unsigned char *) buf) + 80;
    inner_message.Serialize(buf_inner);
    size_t inner_size = inner_message.SerializedSize();

    memset(buf_id, 0, 16);
    ASSERT(identifier.size() < 16);
    identifier.copy(buf_id, identifier.size());
}

#endif

}
