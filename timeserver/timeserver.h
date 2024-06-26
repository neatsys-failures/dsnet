// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * timeserver/timeserver.h:
 *   Timeserver API
 *
 **********************************************************************/

#ifndef _TIME_SERVER_H_
#define _TIME_SERVER_H_

#include "common/replica.h"
#include "lib/configuration.h"
#include "lib/udptransport.h"
#include "replication/fastpaxos/replica.h"
#include "replication/spec/replica.h"
#include "replication/vr/replica.h"

#include <string>

using namespace std;

namespace specpaxos = dsnet;
using namespace dsnet;

class TimeStampServer : public specpaxos::AppReplica {
public:
    TimeStampServer();
    ~TimeStampServer();

    void ReplicaUpcall(
        opnum_t opnum, const string &str1, string &str2, void *arg = nullptr,
        void *ret = nullptr) override;
    void RollbackUpcall(
        opnum_t current, opnum_t to, const std::map<opnum_t, string> &opMap);
    void CommitUpcall(opnum_t op);

private:
    long ts;
    string newTimeStamp();
};
#endif /* _TIME_SERVER_H_ */
