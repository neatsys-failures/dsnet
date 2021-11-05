#pragma once
#include "common/signedadapter.h"

#include <unordered_map>

// Notice: different to QuorumSet, an instance of QuorumCert only
// manages one single slot/seq no.
class QuorumCert {
public:
    QuorumCert(int quorum_size);
    bool AddCheck(int replica_id, const SignedAdapter &message);
    bool Check() const;
    std::unordered_map<int, const SignedAdapter> Export() const;
};
