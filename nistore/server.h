// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * nistore/server.h:
 *   NiStore application server logic
 *
 **********************************************************************/

#ifndef _NI_SERVER_H_
#define _NI_SERVER_H_

#include "common/replica.h"
#include "lib/configuration.h"
#include "lib/udptransport.h"
#include "nistore/lockstore.h"
#include "nistore/occstore.h"
#include "replication/fastpaxos/replica.h"
#include "replication/hotstuff/replica.h"
#include "replication/minbft/replica.h"
#include "replication/pbft/replica.h"
#include "replication/spec/replica.h"
#include "replication/tombft/replica.h"
#include "replication/vr/replica.h"
#include <vector>

namespace specpaxos = dsnet;
using namespace dsnet;

namespace nistore {

using namespace std;

class Server : public specpaxos::AppReplica {
public:
    // set up the store
    Server() { store = OCCStore(); };
    Server(bool locking) { locking ? store = LockStore() : OCCStore(); };
    ~Server(){};
    void ReplicaUpcall(
        opnum_t opnum, const string &str1, string &str2, void *arg = nullptr,
        void *ret = nullptr) override;
    void RollbackUpcall(
        opnum_t current, opnum_t to, const std::map<opnum_t, string> &opMap);
    void CommitUpcall(opnum_t opnum);

private:
    // data store
    TxnStore store;

    struct Operation {
        long id;                  // client ID
        string op;                // requested operation
        std::vector<string> args; // arguments
    };

    Operation parse(string str);
    vector<string> split(string str);
};

} // namespace nistore

#endif /* _NI_SERVER_H_ */
