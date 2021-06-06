#include "lib/message.h"
#include "transaction/eris/eris-proto.pb.h"
#include "transaction/eris/message.h"

namespace dsnet {
namespace transaction {
namespace eris {

/*
 * Packet format:
 * sequencer header size  + sess num + number of groups + each (group id + msg num)
 */
typedef uint16_t HeaderSize;
typedef uint8_t NumGroups;

size_t
Multistamp::SerializedSize() const
{
    return sizeof(SessNum) + sizeof(NumGroups) +
        msg_nums.size() * (sizeof(GroupID) + sizeof(MsgNum));
}

ErisMessage::ErisMessage(::google::protobuf::Message &msg)
    : PBMessage(msg) { }

ErisMessage::ErisMessage(::google::protobuf::Message &msg, const std::vector<int> &groups)
    : PBMessage(msg)
{
    for (const int group : groups) {
        stamp_.msg_nums[group] = 0;
    }
}

ErisMessage::~ErisMessage() { }

size_t
ErisMessage::SerializedSize() const
{
    size_t sz = sizeof(HeaderSize);
    if (stamp_.msg_nums.size() > 0) {
        sz += stamp_.SerializedSize();
    }
    return sz + PBMessage::SerializedSize();
}

void
ErisMessage::Parse(const void *buf, size_t size)
{
    const char *p = (const char*)buf;
    HeaderSize header_sz = *(HeaderSize *)p;
    p += sizeof(HeaderSize);
    if (header_sz > 0) {
        stamp_.sess_num = *(SessNum *)p;
        p += sizeof(SessNum);
        NumGroups n_groups = *(NumGroups *)p;
        p += sizeof(NumGroups);
        for (int i = 0; i < n_groups; i++) {
            GroupID id = *(GroupID *)p;
            p += sizeof(GroupID);
            MsgNum msg_num = *(MsgNum *)p;
            p += sizeof(MsgNum);
            stamp_.msg_nums[id] = msg_num;
        }
    }
    PBMessage::Parse(p, size - sizeof(HeaderSize) - header_sz);
    if (header_sz > 0) {
        // Only client request message will be tagged with multistamp
        proto::ToServerMessage &to_server =
            dynamic_cast<proto::ToServerMessage &>(*msg_);
        if (to_server.msg_case() !=
                proto::ToServerMessage::MsgCase::kRequest) {
            Panic("Received multistamp with wrong message type");
        }
        dsnet::Request *request = to_server.mutable_request()->mutable_request();
        request->set_sessnum(stamp_.sess_num);
        for (auto it = request->mutable_ops()->begin();
                it != request->mutable_ops()->end();
                it++) {
            it->set_msgnum(stamp_.msg_nums.at(it->shard()));
        }
    }
}

void
ErisMessage::Serialize(void *buf) const
{
    char *p = (char *)buf;
    *(HeaderSize *)p = stamp_.msg_nums.size() > 0 ? stamp_.SerializedSize() : 0;
    p += sizeof(HeaderSize);
    if (stamp_.msg_nums.size() > 0) {
        // sess num filled by sequencer
        p += sizeof(SessNum);
        *(NumGroups *)p = stamp_.msg_nums.size();
        p += sizeof(NumGroups);
        for (const auto &kv : stamp_.msg_nums) {
            *(GroupID *)p = kv.first;
            p += sizeof(GroupID);
            // msg num filled by sequencer
            p += sizeof(MsgNum);
        }
    }
    PBMessage::Serialize(p);
}

const Multistamp &
ErisMessage::GetStamp() const
{
    return stamp_;
}

} // namespace eris
} // namespace transaction
} // namespace dsnet
