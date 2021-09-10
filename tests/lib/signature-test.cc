#include "lib/signature.h"

#include <gtest/gtest.h>

#include <string>

#include "lib/configuration.h"

using namespace std;
using namespace dsnet;
TEST(Signature, CanSignAndVerify) {
  std::string message = "Hello!";
  Secp256k1Signer signer;
  Secp256k1Verifier verifier(signer);
  HomogeneousSecurity sec(signer, verifier);
  std::string signature;
  ASSERT_TRUE(sec.ReplicaSigner(0).Sign(message, signature));
  ASSERT_GT(signature.size(), 0);
  ASSERT_TRUE(sec.ReplicaVerifier(0).Verify(message, signature));
}

TEST(Signature, MultipleSignAndVerify) {
  std::string hello = "Hello!", bye = "Goodbye!";
  Secp256k1Signer signer;
  Secp256k1Verifier verifier(signer);
  HomogeneousSecurity sec(signer, verifier);
  std::string helloSig, byeSig;
  ASSERT_TRUE(sec.ReplicaSigner(0).Sign(hello, helloSig));
  ASSERT_TRUE(sec.ReplicaSigner(0).Sign(bye, byeSig));
  ASSERT_TRUE(sec.ReplicaVerifier(0).Verify(hello, helloSig));
  ASSERT_TRUE(sec.ReplicaVerifier(0).Verify(hello, helloSig));
  ASSERT_TRUE(sec.ReplicaVerifier(0).Verify(bye, byeSig));
  ASSERT_FALSE(sec.ReplicaVerifier(0).Verify(hello, byeSig));
  ASSERT_FALSE(sec.ReplicaVerifier(0).Verify(bye, helloSig));
}

extern "C" int halfsiphash(const void *in, const size_t inlen, const void *k,
                           uint8_t *out, const size_t outlen);

TEST(Signature, HalfSipHash101) {
  // uint8_t k[8] = {0x33, 0x32, 0x31, 0x30, 0x42, 0x41, 0x39, 0x38};
  uint8_t k[8] = {0x30, 0x31, 0x32, 0x33, 0x38, 0x39, 0x41, 0x42};
  uint8_t out[4];
  halfsiphash(
      "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff", 16, k,
      out, 4);
  printf("%x%x%x%x\n", out[0], out[1], out[2], out[3]);
  ASSERT_TRUE(memcmp(out, "\xdf\x8f\x33\x46", 4) == 0);
}