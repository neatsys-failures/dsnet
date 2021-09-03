// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * nistore/lockstore.h
 *   Key-value store with support for transactions using S2PL
 *
 **********************************************************************/

#ifndef _NI_LOCK_STORE_H_
#define _NI_LOCK_STORE_H_

#include "nistore/kvstore.h"
#include "nistore/lockserver.h"
#include "nistore/txnstore.h"
#include "lib/viewstamp.h"
#include <vector>
#include <unordered_map>
#include <set>
#include <map>
#include <list>

namespace nistore {

using namespace std;

class LockStore : public TxnStore
{
public:
    LockStore();
    ~LockStore();

    // begin a transaction
    virtual void begin(uint64_t id);
    // add key to read set
    virtual int get(uint64_t id, const string &key, string &value);
    // add key to write set
    virtual int put(uint64_t id, const string &key, const string &value);
    // check whether we can commit or abort this transaction
    // and lock the read/write set
    virtual int prepare(uint64_t id, opnum_t op);
    // commit the transaction
    virtual void commit(uint64_t id, uint64_t timestamp, opnum_t op);
    // abort a running transaction
    virtual void abortTxn(uint64_t id, opnum_t op);

    // undo operations from Spec Paxos
    virtual void unbegin(uint64_t id);
    virtual void unget(uint64_t id, const string &key);
    virtual void unput(uint64_t id, const string &key, const string &value);
    virtual void unprepare(uint64_t id, opnum_t op);
    virtual void uncommit(uint64_t id, uint64_t timestamp, opnum_t op);
    virtual void unabort(uint64_t id, opnum_t op);

    // upcall from Spec Paxos to clean up
    virtual void specCommit(opnum_t op);
    
private:
    // data store
    KVStore store;
    // locks
    LockServer locks;

    // Currently active transaction
    // may be running or prepared
    struct Transaction {
        uint64_t id;
        // list of keys read and number of times each has been read
        map<string, int> readSet;
        // map between key and value(s)
        map<string, list<string>> writeSet;

        Transaction() : id(0) { };
        Transaction(uint64_t i) : id(i) { };

        bool operator== (const Transaction &t);
    };

    enum RetiredState {
        COMMITTED,
        ABORTED_PREPARED,
        ABORTED_RUNNING
    };

    struct RetiredTxn {
        Transaction txn;
        RetiredState state;

        RetiredTxn(Transaction t, RetiredState s) : 
            txn(t), state(s) { };
    };

    map<uint64_t,Transaction> running;
    map<uint64_t,Transaction> prepared;
    list<pair<opnum_t,RetiredTxn>> retired;

    Transaction& getTxn(uint64_t id);
    Transaction getRetiredTxn(opnum_t op, uint64_t id, RetiredState state);
    void dropLocks(const Transaction &txn);
    void getLocks(const Transaction &txn);
};

} // namespace nistore

#endif /* _NI_LOCK_STORE_H_ */
