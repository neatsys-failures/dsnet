#pragma once
#include "common/signedadapter.h"
#include "lib/assert.h"
#include "lib/transport.h"

namespace dsnet {
namespace minbft {

class MinBFTPlainAdapter : public Message {
    MinBFTPlainAdapter(Message *inner, bool assign_ui) : inner(inner) {
        if (assign_ui) {
            s_ui += 1;
            ui = s_ui;
        } else {
            ui = 0;
        }
    }

    void Parse(const void *buf, size_t len) override {
        ui = ((opnum_t *)buf)[0];
        const std::uint8_t *inner_buf =
            (const std::uint8_t *)buf + sizeof(opnum_t);
        size_t inner_len = len - sizeof(opnum_t);

        inner->Parse(inner_buf, inner_len);
    }
    size_t SerializedSize() const override {
        return inner->SerializedSize() + sizeof(opnum_t);
    }
    void Serialize(void *buf) const override {
        ((opnum_t *)buf)[0] = ui;
        std::uint8_t *inner_buf = (std::uint8_t *)buf + sizeof(opnum_t);

        inner->Serialize(inner_buf);
    }
    // have to "implement" all interface so the object can be constructed
    Message *Clone() const override { NOT_IMPLEMENTED(); }
    std::string Type() const override { NOT_IMPLEMENTED(); }

protected:
    static opnum_t s_ui;

    Message *inner;
    opnum_t ui;

    friend class MinBFTAdapter;
};

// MinBFT[m] = Signed[MinBFTPlain[m]]
class MinBFTAdapter : public Message {
public:
    // the subclass of `Message` is getting more and more weird...
    MinBFTAdapter(
        Message *inner, const std::string &identifier, bool assign_ui);
    void SetInner(Message *inner) { plain_layer.inner = inner; }

    MinBFTAdapter *Clone() const override {
        MinBFTAdapter *cloned = new MinBFTAdapter(
            plain_layer.inner, signed_layer.Identifier(), false);
        cloned->plain_layer.ui = plain_layer.ui;
        return cloned;
    }
    std::string Type() const override {
        return "MinBFT[" + plain_layer.inner->Type() + "]";
    }
    // most part of MinBFTAdapter is implemented by composing SignedAdapter
    size_t SerializedSize() const override {
        return signed_layer.SerializedSize();
    }

    void Parse(const void *buf, size_t len) override {
        signed_layer.Parse(buf, len);
    }
    // only vaid when `assign_ui` == true
    void Serialize(void *buf) const override { signed_layer.Serialize(buf); }

    // for sender, must call when `assign_ui` == true
    // for receiver, must call after `Parse`
    opnum_t GetUI() const { return plain_layer.ui; }
    // only valid after call `Parse`
    bool IsVerified() const { return signed_layer.IsVerified(); }

private:
    mutable MinBFTPlainAdapter plain_layer;
    mutable SignedAdapter signed_layer;
};

} // namespace minbft
} // namespace dsnet