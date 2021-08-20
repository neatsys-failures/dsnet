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
  PbftTestApp app;
  MinBFTReplica replica(c, 0, true, &t, s, &app);
  MinBFTClient client(c, ReplicaAddress("localhost", "0"), &t, s);
}