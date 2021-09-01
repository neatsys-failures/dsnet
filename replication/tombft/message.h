#pragma once

#include <arpa/inet.h>
#include <endian.h>
#include <openssl/sha.h>

#include "common/pbmessage.h"
#include "lib/message.h"
#include "lib/signature.h"

#define HMAC_LENGTH SHA256_DIGEST_LENGTH
#define DIGEST_LENGTH SHA256_DIGEST_LENGTH

#define HTON_SESSNUM(n) htons(n)
#define NTOH_SESSNUM(n) ntohs(n)
#define HTON_MSGNUM(n) htobe64(n)
#define NTOH_MSGNUM(n) be64toh(n)

namespace dsnet {
namespace tombft {

class TomBFTMessage : public Message {
 public:
  struct __attribute__((packed)) Header {
    std::uint16_t sess_num;
    std::uint64_t msg_num;
    union {
      char digest[16];
      char hmac[16];
    } sig_list[4];
  };
  Header meta;

  TomBFTMessage(::google::protobuf::Message &msg, bool sequencing = false)
      : pb_msg(PBMessage(msg)), sequencing(sequencing) {
    meta.sess_num = 0;
  }
  ~TomBFTMessage() {}

 private:
  TomBFTMessage(const TomBFTMessage &msg)
      : meta(msg.meta),
        pb_msg(*std::unique_ptr<PBMessage>(msg.pb_msg.Clone())),
        sequencing(msg.sequencing) {}

 public:
  virtual TomBFTMessage *Clone() const override {
    return new TomBFTMessage(*this);
  }
  virtual std::string Type() const override { return pb_msg.Type(); }
  virtual size_t SerializedSize() const override {
    return (sequencing ? sizeof(Header) : sizeof(size_t)) +
           pb_msg.SerializedSize();
  }
  virtual void Parse(const void *buf, size_t size) override;
  virtual void Serialize(void *buf) const override;

 private:
  PBMessage pb_msg;
  bool sequencing;
};

}  // namespace tombft
}  // namespace dsnet
