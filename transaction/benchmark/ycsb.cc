// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * store/benchmark/ycsb.cc:
 *   Benchmarking client for a distributed transactional kv-store.
 *
 **********************************************************************/

#include <vector>
#include <map>
#include <atomic>
#include <algorithm>

#include "lib/latency.h"
#include "lib/timeval.h"
#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/udptransport.h"
#include "lib/dpdktransport.h"
#include "transaction/common/frontend/txnclientcommon.h"
#include "transaction/apps/kvstore/client.h"
#include "transaction/eris/client.h"
#include "transaction/granola/client.h"
#include "transaction/unreplicated/client.h"
#include "transaction/spanner/client.h"
#include "transaction/tapir/client.h"
#include "transaction/benchmark/header.h"

using namespace std;
using namespace dsnet;
using namespace dsnet::transaction;
using namespace dsnet::transaction::kvstore;

DEFINE_LATENCY(op);

// Function to pick a random key according to some distribution.
static int rand_key();
static int rand_value();

static bool ready = false;
static double alpha = -1;
static double *zipf;

static vector<string> keys;
static vector<string> values;
static int nKeys = 1000;
static int nValues = 1000;

static int duration = 10;
static int tputInterval = 0;
static int readportion = 50;
static int updateportion = 50;
static int rmwportion = 0;
static bool indep = true;
static dsnet::Transport *transport;
static int n_threads = 1;

static std::vector<uint64_t> total_latency;
static std::vector<uint64_t> commit_transactions;
static std::vector<uint64_t> total_transactions;
static std::vector<std::map<uint64_t, int>> throughputs, latency_dist;
static std::atomic<int> n_finished(0);

static int rand_value();

static void
client_thread(int tid, int core, KVClient *kvClient, TxnClient *txnClient, Client *protoClient, bool pin)
{
    if (pin) {
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET(core, &cs);
        if (sched_setaffinity(0, sizeof(cs), &cs) != 0) {
            Panic("Failed to pin client thread to cpu core");
        }
    }
    transport->SetCoreID(core);
    phase_t phase = WARMUP;
    uint64_t last_interval_txns = 0;
    struct timeval initialTime, currTime, lastInterval;
    struct Latency_t latency;

    _Latency_Init(&latency, "op");
    gettimeofday(&initialTime, NULL);
    while (true) {
        gettimeofday(&currTime, NULL);
        uint64_t time_elapsed = currTime.tv_sec - initialTime.tv_sec;

        if (phase == MEASURE && tputInterval > 0) {
            int time_since_interval = (currTime.tv_sec - lastInterval.tv_sec)*1000 +
                (currTime.tv_usec - lastInterval.tv_usec)/1000;
            if (time_since_interval >= tputInterval) {
                uint64_t index = ((currTime.tv_sec*1000+currTime.tv_usec/1000)
                        /tputInterval)*tputInterval;
                throughputs[tid][index] += (commit_transactions[tid] - last_interval_txns)
                    * (1000 / tputInterval);
                lastInterval = currTime;
                last_interval_txns = commit_transactions[tid];
            }
        }

        if (phase == WARMUP) {
            if (time_elapsed >= (uint64_t)duration / 3) {
                phase = MEASURE;
                gettimeofday(&lastInterval, NULL);
            }
        } else if (phase == MEASURE) {
            if (time_elapsed >= (uint64_t)duration * 2 / 3) {
                phase = COOLDOWN;
            }
        } else if (phase == COOLDOWN) {
            if (time_elapsed >= (uint64_t)duration) {
                break;
            }
        }

        Latency_Start(&latency);

        int ttype = rand() % 100;
        bool commit;

        if (ttype < readportion) {
            string key = keys[rand_key()];
            string value;
            commit = kvClient->InvokeGetTxn(key, value);
        } else if (ttype < readportion + updateportion) {
            string key = keys[rand_key()];
            string value = values[rand_value()];
            commit = kvClient->InvokePutTxn(key, value);
        } else {
            string key1 = keys[rand_key()];
            string key2 = keys[rand_key()];
            string value1 = values[rand_value()];
            string value2 = values[rand_value()];
            commit = kvClient->InvokeRMWTxn(key1, key2, value1, value2, indep);
        }

        uint64_t ns = Latency_End(&latency);

        if (phase == MEASURE) {
            if (commit) {
                commit_transactions[tid]++;
            }
            latency_dist[tid][ns/1000]++;
            total_transactions[tid]++;
            total_latency[tid] += ns/1000;
        }
    }
    txnClient->Done();
    if (++n_finished == n_threads) {
        transport->Stop();
    }
}

int
main(int argc, char **argv)
{
    const char *configPath = nullptr;
    const char *keysPath = nullptr;
    const char *valuesPath = nullptr;
    int nShards = 1;
    string host, dev, transport_cmdline, stats_file;
    int dev_port = 0;
    int n_transport_cores = 1;

    protomode_t mode = PROTO_UNKNOWN;
    enum { TRANSPORT_UDP, TRANSPORT_DPDK } transport_type = TRANSPORT_UDP;

    int opt;
    while ((opt = getopt(argc, argv, "c:d:e:f:gh:i:k:m:N:p:r:s:t:T:u:v:w:x:z:Z:")) != -1) {
        switch (opt) {
        case 'c': // Configuration path
        {
            configPath = optarg;
            break;
        }

        case 'd': // duration to run
        {
            char *strtolPtr;
            duration = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                    (duration <= 0)) {
                fprintf(stderr, "option -d requires a numeric arg > 0\n");
            }
            break;
        }

        case 'h': // host
            host = string(optarg);
            break;

        case 'e': // device
        {
            dev = string(optarg);
            break;
        }

        case 'x': // device port
        {
            char *strtol_ptr;
            dev_port = strtoul(optarg, &strtol_ptr, 10);
            if ((*optarg == '\0') || (*strtol_ptr != '\0')) {
                fprintf(stderr, "option -x requires a numeric arg\n");
            }
            break;
        }

        case 'Z': // transport command line
        {
            transport_cmdline = string(optarg);
            break;
        }

        case 'p':
        {
            if (strcasecmp(optarg, "udp") == 0) {
                transport_type = TRANSPORT_UDP;
            } else if (strcasecmp(optarg, "dpdk") == 0) {
                transport_type = TRANSPORT_DPDK;
            } else {
                fprintf(stderr, "unknown transport '%s'\n", optarg);
            }
            break;
        }

        case 'N': // Number of shards.
        {
            char *strtolPtr;
            nShards = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                    (nShards <= 0)) {
                fprintf(stderr, "option -n requires a numeric arg\n");
            }
            break;
        }

        case 'k': // Number of keys to operate on.
        {
            char *strtolPtr;
            nKeys = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                    (nKeys <= 0)) {
                fprintf(stderr, "option -k requires a numeric arg\n");
            }
            break;
        }

        case 'f': // Generated keys path
        {
            keysPath = optarg;
            break;
        }

        case 'v': // Generated values path
        {
            valuesPath = optarg;
            break;
        }

        case 'm': // Mode to run in [occ/lock/...]
        {
            if (strcasecmp(optarg, "eris") == 0) {
                mode = PROTO_ERIS;
            } else if (strcasecmp(optarg, "granola") == 0) {
                mode = PROTO_GRANOLA;
            } else if (strcasecmp(optarg, "unreplicated") == 0) {
                mode = PROTO_UNREPLICATED;
            } else if (strcasecmp(optarg, "spanner") == 0) {
                mode = PROTO_SPANNER;
            } else if (strcasecmp(optarg, "tapir") == 0) {
                mode = PROTO_TAPIR;
            } else {
                fprintf(stderr, "Unknown protocol mode %s\n", optarg);
            }
            break;
        }

        case 'z': // Zipf coefficient for key selection.
        {
            char *strtolPtr;
            alpha = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0'))
            {
                fprintf(stderr,
                        "option -z requires a numeric arg\n");
            }
            break;
        }

        case 'i': // Throughput measurement interval
        {
            char *strtolPtr;
            tputInterval = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || tputInterval <= 0)
            {
                fprintf(stderr,
                        "option -i requires a numeric arg > 0\n");
            }
            break;
        }

        case 'r': // read portion
        {
            char *strtolPtr;
            readportion = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || readportion < 0 || readportion > 100)
            {
                fprintf(stderr,
                        "option -r requires a numeric arg between 0 and 100\n");
            }
            break;
        }

        case 'u': // update portion
        {
            char *strtolPtr;
            updateportion = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || updateportion < 0 || updateportion > 100)
            {
                fprintf(stderr,
                        "option -u requires a numeric arg between 0 and 100\n");
            }
            break;
        }

        case 'w': // rmw portion
        {
            char *strtolPtr;
            rmwportion = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || rmwportion < 0 || rmwportion > 100)
            {
                fprintf(stderr,
                        "option -w requires a numeric arg between 0 and 100\n");
            }
            break;
        }

        case 'g': // general transactions
        {
            indep = false;
            break;
        }

        case 's': // statistics file
        {
            stats_file = string(optarg);
            break;
        }

        case 't': // number of benchmark threads (per transport core)
        {
            char *strtolPtr;
            n_threads = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || n_threads < 1)
            {
                fprintf(stderr,
                        "option -t requires a numeric arg >= 1\n");
            }
            break;
        }

        case 'T': // number of transport cores
        {
            char *strtolPtr;
            n_transport_cores = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || n_transport_cores < 1)
            {
                fprintf(stderr,
                        "option -T requires a numeric arg >= 1\n");
            }
            break;
        }

        default:
            fprintf(stderr, "Unknown argument %s\n", argv[optind]);
            break;
        }
    }

    if (configPath == nullptr) {
        Panic("option -c required");
    }

    if (host.empty()) {
        Panic("tpccClient requires -h option\n");
    }

    if (mode == PROTO_UNKNOWN) {
        Panic("option -m required");
    }

    if (readportion + updateportion + rmwportion != 100) {
        Panic("Workload portions should add up to 100");
    }

    // Initialize random number seed
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec);

    // Initialize transport
    ifstream configStream(configPath);
    if (configStream.fail()) {
        Panic("unable to read configuration file: %s", configPath);
    }

    Configuration config(configStream);
    ReplicaAddress addr(host, "0", dev);

    switch (transport_type) {
        case TRANSPORT_UDP:
            transport = new dsnet::UDPTransport(0, 0);
            break;
        case TRANSPORT_DPDK:
            transport = new dsnet::DPDKTransport(dev_port, 0,
                    n_transport_cores, transport_cmdline);
            break;
    }

    // Read in the keys from a file.
    string inkey, invalue;
    ifstream in;
    in.open(keysPath);
    if (!in) {
        fprintf(stderr, "Could not read keys from: %s\n", keysPath);
        exit(0);
    }
    for (int i = 0; i < nKeys; i++) {
        getline(in, inkey);
        keys.push_back(inkey);
    }
    in.close();
    in.open(valuesPath);
    if (!in) {
        fprintf(stderr, "Could not read values from: %s\n", valuesPath);
        exit(0);
    }
    for (int i = 0; i < nValues; i++) {
        getline(in, invalue);
        values.push_back(invalue);
    }
    in.close();

    // Start benchmark threads
    total_latency.resize(n_transport_cores * n_threads, 0);
    commit_transactions.resize(n_transport_cores * n_threads, 0);
    total_transactions.resize(n_transport_cores * n_threads, 0);
    throughputs.resize(n_transport_cores * n_threads);
    latency_dist.resize(n_transport_cores * n_threads);
    std::vector<std::thread> threads;
    std::vector<KVClient *> kvclients;
    std::vector<Client *> protoclients;
    for (int c = 0; c < n_transport_cores; c++) {
        for (int i = 0; i < n_threads; i++) {
            KVClient *kvClient = nullptr;
            TxnClient *txnClient = nullptr;
            Client *protoClient = nullptr;

            switch (mode) {
                case PROTO_ERIS:
                    protoClient = new eris::ErisClient(config, addr, transport);
                    break;
                case PROTO_GRANOLA:
                    protoClient = new granola::GranolaClient(config, addr, transport);
                    break;
                case PROTO_UNREPLICATED:
                    protoClient =
                        new transaction::unreplicated::UnreplicatedClient(config,
                                addr, transport);
                    break;
                case PROTO_SPANNER:
                    protoClient = new spanner::SpannerClient(config, addr, transport);
                    break;
                case PROTO_TAPIR:
                    break;
                default:
                    Panic("Unknown protocol mode");
            }
            if (mode == PROTO_TAPIR) {
                txnClient = new tapir::TapirClient(config, addr, transport);
            } else {
                txnClient = new TxnClientCommon(transport, protoClient);
            }
            kvClient = new KVClient(txnClient, nShards);
            kvclients.push_back(kvClient);
            if (protoClient) {
                protoclients.push_back(protoClient);
            }
            bool pin = transport_type == TRANSPORT_DPDK;
            threads.push_back(std::thread(client_thread,
                        c * n_threads + i, c, kvClient, txnClient, protoClient, pin));
        }
    }
    transport->Run();
    for (auto &t : threads) {
        t.join();
    }
    for (KVClient *c : kvclients) {
        delete c; // destructor of kvClient will deallocate txnClient
    }
    for (Client *c : protoclients) {
        delete c;
    }

    // Combine results
    uint64_t agg_total_latency = 0,
             agg_commit_transactions = 0,
             agg_total_transactions = 0;
    std::map<uint64_t, int> agg_throughputs, agg_latency_dist;
    for (int i = 0; i < n_transport_cores * n_threads; i++) {
        agg_total_latency += total_latency[i];
        agg_commit_transactions += commit_transactions[i];
        agg_total_transactions += total_transactions[i];
        for (const auto &kv : throughputs[i]) {
            agg_throughputs[kv.first] += kv.second;
        }
        for (const auto &kv : latency_dist[i]) {
            agg_latency_dist[kv.first] += kv.second;
        }
    }

    Notice("Completed %lu transactions in %.3f seconds",
           agg_commit_transactions, (float)duration / 3);
    Notice("Commit rate %.3f", (double)agg_commit_transactions / agg_total_transactions);

    Notice("Average latency is %lu us", agg_total_latency/agg_total_transactions);
    enum class Mode { kMedian, k90, k95, k99 };
    Mode m = Mode::kMedian;
    uint64_t sum = 0;
    uint64_t median, p90, p95, p99;
    for (auto kv : agg_latency_dist) {
        sum += kv.second;
        switch (m) {
            case Mode::kMedian:
                if (sum >= agg_total_transactions / 2) {
                    median = kv.first;
                    Notice("Median latency is %ld", median);
                    m = Mode::k90;
                    // fall through
                } else {
                    break;
                }
            case Mode::k90:
                if (sum >= agg_total_transactions * 90 / 100) {
                    p90 = kv.first;
                    Notice("90th percentile latency is  latency is %ld", p90);
                    m = Mode::k95;
                    // fall through
                } else {
                    break;
                }
            case Mode::k95:
                if (sum >= agg_total_transactions * 95 / 100) {
                    p95 = kv.first;
                    Notice("95th percentile latency is  latency is %ld", p95);
                    m = Mode::k99;
                    // fall through
                } else {
                    break;
                }
            case Mode::k99:
                if (sum >= agg_total_transactions * 99 / 100) {
                    p99 = kv.first;
                    Notice("99th percentile latency is  latency is %ld", p99);
                    goto done;
                } else {
                    break;
                }
        }
    }
done:
    if (stats_file.size() > 0) {
        std::ofstream fs(stats_file.c_str(), std::ios::out);
        fs << agg_commit_transactions / ((float)duration / 3) << std::endl;
        fs << median << " " << p90 << " " << p95 << " " << p99 << std::endl;
        for (const auto &kv : agg_latency_dist) {
            fs << kv.first << " " << kv.second << std::endl;
        }
        fs.close();
        if (agg_throughputs.size() > 0) {
            fs.open(stats_file.append("_tputs").c_str());
            for (const auto &kv : agg_throughputs) {
                fs << kv.first << " " << kv.second << std::endl;
            }
            fs.close();
        }
    }

    delete transport;

    return 0;
}

int rand_key()
{
    if (alpha < 0) {
        // Uniform selection of keys.
        return (rand() % nKeys);
    } else {
        // Zipf-like selection of keys.
        if (!ready) {
            zipf = new double[nKeys];

            double c = 0.0;
            for (int i = 1; i <= nKeys; i++) {
                c = c + (1.0 / pow((double) i, alpha));
            }
            c = 1.0 / c;

            double sum = 0.0;
            for (int i = 1; i <= nKeys; i++) {
                sum += (c / pow((double) i, alpha));
                zipf[i - 1] = sum;
            }
            ready = true;
        }

        double random = 0.0;
        while (random == 0.0 || random == 1.0) {
            random = (1.0 + rand()) / RAND_MAX;
        }

        // binary search to find key;
        int l = 0, r = nKeys, mid;
        while (l < r) {
            mid = (l + r) / 2;
            if (random > zipf[mid]) {
                l = mid + 1;
            } else if (random < zipf[mid]) {
                r = mid - 1;
            } else {
                break;
            }
        }
        return mid;
    }
}

int rand_value()
{
    // Uniform selection of values.
    return (rand() % nValues);
}
