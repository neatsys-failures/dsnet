#include "common/signedadapter.h"
#include <gtest/gtest.h>

using namespace dsnet;
using namespace std;

class StringMessage : public Message {
public:
    string content;

    Message *Clone() const override {
        StringMessage *message = new StringMessage();
        message->content = content;
        return message;
    }
    std::string Type() const override {
        return "StringMessage";
    }
    size_t SerializedSize() const override {
        return content.size();
    }
    void Parse(const void *buf, size_t size) override {
        content.assign((const char *) buf, size);
    }
    void Serialize(void *buf) const override {
        content.copy((char *) buf, content.size());
    }
};

TEST(SignedAdapter, Basic) {
    StringMessage inner;
    inner.content = "Hello!";
    SignedAdapter message(inner, "Steve");
    size_t dump_size = message.SerializedSize();
    char *buf = new char[dump_size];
    message.Serialize(buf);

    StringMessage recv_inner;
    SignedAdapter recv_message(recv_inner, "Steve");
    recv_message.Parse(buf, dump_size);
    ASSERT_TRUE(recv_message.IsVerified());
    ASSERT_NE(recv_message.Digest(), "");
    ASSERT_EQ(recv_inner.content, "Hello!");

    buf[dump_size - 1] = '?';  // Hello! -> Hello?
    recv_message.Parse(buf, dump_size);
    ASSERT_FALSE(recv_message.IsVerified());
    delete[] buf;
}
