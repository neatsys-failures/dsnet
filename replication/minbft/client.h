#pragma once

#include "common/client.h"
#include "lib/assert.h"
#include "lib/signature.h"

namespace dsnet {
namespace minbft {
class MinBFTClient : public Client {
 public:
  MinBFTClient(const Configuration &c, ReplicaAddress &addr, Transport *t,
               const Security &s);
  virtual ~MinBFTClient();
  virtual void ReceiveMessage(const TransportAddress &remote, void *buf,
                              size_t len) override;
  virtual void Invoke(const string &op, continuation_t then) override;
  virtual void InvokeUnlogged(
      int replica_id, const string &op, continuation_t then,
      timeout_continuation_t timeout_then,
      uint32_t timeout = DEFAULT_UNLOGGED_OP_TIMEOUT) override {
    NOT_IMPLEMENTED();
  }

private:
  opnum_t op_num = 0;
  continuation_t then;
};
}  // namespace minbft
}  // namespace dsnet