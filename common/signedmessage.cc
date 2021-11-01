#include "common/signedmessage.h"
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
static secp256k1_context *PROTO_CTX_SIGN, *PROTO_CTX_VERIFY;

SignedMessage::SignedMessage(Message &inner_message, const string identifier) 
    : inner_message(inner_message), identifier(identifier) 
{
}

Message *SignedMessage::Clone() const {
    SignedMessage *message = new SignedMessage(inner_message, identifier);
    message->is_verified = is_verified;
    message->digest = digest;
    return message;
}

string SignedMessage::Type() const {
    return inner_message.Type();
}

size_t SignedMessage::SerializedSize() const {
    // sign overhead: 64B signature + 16B identifier
    return inner_message.SerializedSize() + 80;
}

void SignedMessage::Parse(const void *buf, size_t size) {
    const char *identifier = (const char *) buf;
    const unsigned char *buf_sig = ((const unsigned char *) buf) + 16;
    const unsigned char *buf_inner = ((const unsigned char *) buf) + 80;
    size_t inner_size = size - 80;
    ASSERT(identifier[15] == '\0');  // for develop debug
    ASSERT(strcmp(identifier, "Steve") == 0);  // for develop debug

    if (PROTO_CTX_VERIFY == nullptr) {
        PROTO_CTX_VERIFY = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    }
    secp256k1_context *ctx = secp256k1_context_clone(PROTO_CTX_VERIFY);
    secp256k1_pubkey pubkey;
    int code = secp256k1_ec_pubkey_create(ctx, &pubkey, STEVE_SECKEY);
    ASSERT(code);
    secp256k1_ecdsa_signature sig;
    unsigned char *digest;
    if (!secp256k1_ecdsa_signature_parse_compact(ctx, &sig, buf_sig)) {
        is_verified = false;
        goto finalize;
    }
    digest = SHA256(buf_inner, inner_size, new unsigned char[32]);
    this->digest.assign((char *)digest, 32);
    is_verified = secp256k1_ecdsa_verify(ctx, &sig, digest, &pubkey) == 1;
    delete[] digest;
    if (is_verified) {
        inner_message.Parse(buf_inner, inner_size);
    }
finalize:
    secp256k1_context_destroy(ctx);
}

void SignedMessage::Serialize(void *buf) const {
    char *identifier = (char *)buf;
    unsigned char *buf_sig = ((unsigned char *) buf) + 16;
    unsigned char *buf_inner = ((unsigned char *) buf) + 80;
    inner_message.Serialize(buf_inner);
    size_t inner_size = inner_message.SerializedSize();

    memset(identifier, 0, 16);
    strcpy(identifier, "Steve");
    if (PROTO_CTX_SIGN == nullptr) {
        PROTO_CTX_SIGN = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    }
    secp256k1_context *ctx = secp256k1_context_clone(PROTO_CTX_SIGN);
    unsigned char *digest = SHA256(buf_inner, inner_size, new unsigned char[32]);
    secp256k1_ecdsa_signature sig;
    int code = secp256k1_ecdsa_sign(ctx, &sig, digest, STEVE_SECKEY, nullptr, nullptr);
    ASSERT(code);
    delete[] digest;
    code = secp256k1_ecdsa_signature_serialize_compact(ctx, buf_sig, &sig);
    secp256k1_context_destroy(ctx);
}

}
