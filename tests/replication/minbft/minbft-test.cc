#include <gtest/gtest.h>

#include "lib/simtransport.h"
#include "replication/minbft/client.h"
#include "replication/minbft/replica.h"

using namespace std;
using namespace dsnet;
using namespace dsnet::minbft;

class PbftTestApp : public AppReplica {
 public:
  vector<string> opList;
  string LastOp() { return opList.back(); }

  PbftTestApp(){};
  ~PbftTestApp(){};

  void ReplicaUpcall(opnum_t opnum, const string &req, string &reply,
                     void *arg = nullptr, void *ret = nullptr) override {
    opList.push_back(req);
    reply = "reply: " + req;
  }
};

Configuration c(1, 3, 1,
                {{0,
                  {{"localhost", "1509"},
                   {"localhost", "1510"},
                   {"localhost", "1511"},
                   {"localhost", "1512"}}}});
TEST(MinBFT, SetUp) {
  SimulatedTransport t;
  NopSecurity s;
  PbftTestApp app[3];
  MinBFTReplica replica0(c, 0, true, &t, s, &app[0]);
  MinBFTReplica replica1(c, 1, true, &t, s, &app[1]);
  MinBFTReplica replica2(c, 2, true, &t, s, &app[2]);
  MinBFTClient client(c, ReplicaAddress("localhost", "0"), &t, s);
  client.Invoke("hello", [&](const string &req, const string &reply) {
    ASSERT_STREQ(reply.c_str(), "reply: hello");
    t.CancelAllTimers();
  });
  t.Timer(0, [&]() { ASSERT(false); });
  t.Run();
  ASSERT_EQ(app[0].LastOp(), "hello");
  ASSERT_EQ(app[1].LastOp(), "hello");
  ASSERT_EQ(app[2].LastOp(), "hello");
}

TEST(MinBFT, 100Op) {
  SimulatedTransport t;
  Secp256k1Signer signer;
  Secp256k1Verifier verifier(signer);
  HomogeneousSecurity s(signer, verifier);
  // NopSecurity s;
  PbftTestApp app[3];
  MinBFTReplica replica0(c, 0, true, &t, s, &app[0]);
  MinBFTReplica replica1(c, 1, true, &t, s, &app[1]);
  MinBFTReplica replica2(c, 2, true, &t, s, &app[2]);
  MinBFTClient client(c, ReplicaAddress("localhost", "0"), &t, s);
  int n = 0;
  Client::continuation_t upcall = [&](const string &req, const string &reply) {
    char buf[32];
    snprintf(buf, 32, "reply: test%d", n);
    ASSERT_EQ(reply, buf);
    n += 1;
    if (n == 100) {
      t.CancelAllTimers();
      return;
    }
    snprintf(buf, 32, "test%d", n);
    client.Invoke(buf, upcall);
  };
  client.Invoke("test0", upcall);
  t.Timer(0, [&]() { ASSERT(false); });
  t.Run();
}