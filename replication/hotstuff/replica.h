#pragma once
#include "common/log.h"
#include "common/pbmessage.h"
#include "common/replica.h"
#include "common/request.pb.h"
#include "common/runner.h"
#include "replication/hotstuff/message.pb.h"

namespace dsnet {
namespace hotstuff {

class HotStuffEntry : public LogEntry {
    HotStuffEntry(opnum_t op_number, const Request &request)
        : LogEntry(viewstamp_t(0, op_number), LOG_STATE_PREPARED, request) {}
    HotStuffEntry(opnum_t op_number)
        : LogEntry(viewstamp_t(0, op_number), LOG_STATE_NOOP, Request()) {}

private:
    friend class HotStuffReplica;
    // proto::QC justify;
};

class HotStuffReplica : public Replica {
public:
    HotStuffReplica(
        const Configuration &config, int index, std::string identifier,
        int n_thread, int batch_size, Transport *transport, AppReplica *app);
    ~HotStuffReplica();

    void ReceiveMessage(
        const TransportAddress &remote, void *buf, size_t length) override;

private:
    // consts
    std::string identifier;
    CTPLRunner runner;
    int batch_size;
    using SignedVote = std::string;
    SignedVote vote0;

    // single states
    std::unique_ptr<Timeout> resend_vote_timeout;
    std::unique_ptr<Timeout> close_batch_timeout;

    std::unique_ptr<proto::QC> generic_qc, locked_qc;
    std::unique_ptr<proto::GenericMessage> pending_generic;

    // aggregated states
    // in-construct QCs, only on primary, op number -> repica id -> partial sig
    // op number is the "hash" of block in vote message, we pretend it could
    // prove content matching for now
    std::unordered_map<uint64_t, std::unordered_map<int, SignedVote>> high_qc;

    struct ClientEntry {
        std::unique_ptr<TransportAddress> remote;
        uint32_t client_request;
        proto::ReplyMessage reply_message;
        bool has_reply;

        ClientEntry(const TransportAddress &remote, opnum_t request_number)
            : remote(remote.clone()), client_request(request_number),
              has_reply(false) {}
    };
    std::unordered_map<uint64_t, ClientEntry> client_table;
    Log log;
    std::map<opnum_t, proto::Block> block_buffer;

    // tolerant faulty leader not implemented
    int GetPrimary() const { return 0; }
    bool IsPrimary() const { return replicaIdx == GetPrimary(); }

    // util state
    std::vector<Runner::Epilogue> epilogue_list;
    void ConcludeEpilogue() {
        runner.RunEpilogue([epilogue_list = this->epilogue_list] {
            for (Runner::Epilogue epilogue : epilogue_list) {
                epilogue();
            }
        });
        epilogue_list.clear();
    }

    void HandleRequest(const TransportAddress &remote, const Request &request);
    void HandleVote(
        const TransportAddress &remote, const proto::VoteMessage &vote,
        const SignedVote &signed_vote);
    void HandleGeneric(
        const TransportAddress &remote, const proto::GenericMessage &generic);

    // the `view` concept is omitted in the final "practical" version of
    // hotstuff, but I cannot think of a better name
    void EnterNextView(const proto::QC &justify);
    void CloseBatch();
    void SendVote(opnum_t op_number);
    void StartNextBatch();
};

} // namespace hotstuff
} // namespace dsnet