// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * client.cpp:
 *   test instantiation of a client application
 *
 * Copyright 2013 Dan R. K. Ports  <drkp@cs.washington.edu>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/

#include "common/client.h"
#include "bench/benchmark.h"
#include "common/pbmessage.h"
#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/dpdktransport.h"
#include "lib/message.h"
#include "lib/udptransport.h"
#include "replication/fastpaxos/client.h"
#include "replication/hotstuff/client.h"
#include "replication/minbft/client.h"
#include "replication/nopaxos/client.h"
#include "replication/pbft/client.h"
#include "replication/signedunrep/client.h"
#include "replication/tombft/client.h"
#include "replication/unreplicated/client.h"
#include "replication/vr/client.h"

#include <fstream>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

using namespace dsnet;
class Recv : public TransportReceiver {
public:
    Recv(Transport *transport, const Configuration &c, const string &host)
        : transport(transport) {
        ReplicaAddress addr(host, "0");
        transport->RegisterAddress(this, c, &addr);
        transport->ListenOnMulticast(this, c);
        _Latency_Init(&l, "switch");
        n = 0;
    }

    void Start() {
        timeval tv;
        gettimeofday(&tv, nullptr);
        uint64_t start = tv.tv_sec * 1000000 + tv.tv_usec;
        Request request;
        request.set_op(string(100, '0'));
        request.set_clientid(0);
        request.set_clientreqid(0);
        while (true) {
            transport->SendMessageToMulticast(this, PBMessage(request));
            gettimeofday(&tv, nullptr);
            if (tv.tv_sec * 1000000 + tv.tv_usec - start > 10 * 1000 * 1000) {
                break;
            }
        }
        exit(0);
    }

    void ReceiveMessage(const TransportAddress &remote, void *buf, size_t len) override {}

private:
    Latency_t l;
    Transport *transport;
    int n;
};

static void Usage(const char *progName) {
    fprintf(
        stderr,
        "usage: %s "
        "[-n requests] [-t threads] [-w warmup-secs] [-s stats-file] [-d "
        "delay-ms] "
        "[-u duration-sec] [-p udp|dpdk] [-v device] [-x device-port] [-z "
        "transport-cmdline] "
        "-c conf-file -h host-address -m "
        "unreplicated|signedunrep|vr|fastpaxos|nopaxos\n",
        progName);
    exit(1);
}

void PrintReply(const string &request, const string &reply) {
    Notice("Request succeeded; got response %s", reply.c_str());
}

int main(int argc, char **argv) {
    const char *configPath = NULL;
    int numClients = 1;
    int duration = 1;
    uint64_t delay = 0;
    int tputInterval = 0;
    std::string host, dev, transport_cmdline;
    int dev_port = 0;

    enum {
        PROTO_UNKNOWN,
        PROTO_UNREPLICATED,
        PROTO_SIGNEDUNREP,
        PROTO_VR,
        PROTO_FASTPAXOS,
        PROTO_SPEC,
        PROTO_NOPAXOS,
        PROTO_TOMBFT,
        PROTO_TOMBFT_HMAC,
        PROTO_HOTSTUFF,
        PROTO_PBFT,
        PROTO_MINBFT
    } proto = PROTO_UNKNOWN;

    enum { TRANSPORT_UDP, TRANSPORT_DPDK } transport_type = TRANSPORT_UDP;

    string statsFile;

    // Parse arguments
    int opt;
    while ((opt = getopt(argc, argv, "c:d:h:s:m:t:i:u:p:v:x:z:")) != -1) {
        switch (opt) {
        case 'c':
            configPath = optarg;
            break;

        case 'd': {
            char *strtolPtr;
            delay = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0')) {
                fprintf(stderr, "option -d requires a numeric arg\n");
                Usage(argv[0]);
            }
            break;
        }

        case 'h':
            host = std::string(optarg);
            break;

        case 'v':
            dev = std::string(optarg);
            break;

        case 'x': {
            char *strtol_ptr;
            dev_port = strtoul(optarg, &strtol_ptr, 10);
            if ((*optarg == '\0') || (*strtol_ptr != '\0')) {
                fprintf(stderr, "option -x requires a numeric arg\n");
                Usage(argv[0]);
            }
            break;
        }

        case 's':
            statsFile = string(optarg);
            break;

        case 'm':
            if (strcasecmp(optarg, "unreplicated") == 0) {
                proto = PROTO_UNREPLICATED;
            } else if (strcasecmp(optarg, "signedunrep") == 0) {
                proto = PROTO_SIGNEDUNREP;
            } else if (strcasecmp(optarg, "vr") == 0) {
                proto = PROTO_VR;
            } else if (strcasecmp(optarg, "fastpaxos") == 0) {
                proto = PROTO_FASTPAXOS;
            } else if (strcasecmp(optarg, "nopaxos") == 0) {
                proto = PROTO_NOPAXOS;
            } else if (strcasecmp(optarg, "tombft") == 0) {
                proto = PROTO_TOMBFT;
            } else if (strcasecmp(optarg, "tombft-hmac") == 0) {
                proto = PROTO_TOMBFT_HMAC;
            } else if (strcasecmp(optarg, "hotstuff") == 0) {
                proto = PROTO_HOTSTUFF;
            } else if (strcasecmp(optarg, "pbft") == 0) {
                proto = PROTO_PBFT;
            } else if (strcasecmp(optarg, "minbft") == 0) {
                proto = PROTO_MINBFT;
            } else {
                fprintf(stderr, "unknown mode '%s'\n", optarg);
                Usage(argv[0]);
            }
            break;

        case 'u': {
            char *strtolPtr;
            duration = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || (duration <= 0)) {
                fprintf(stderr, "option -n requires a numeric arg\n");
                Usage(argv[0]);
            }
            break;
        }

        case 't': {
            char *strtolPtr;
            numClients = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                (numClients <= 0)) {
                fprintf(stderr, "option -t requires a numeric arg\n");
                Usage(argv[0]);
            }
            break;
        }

        case 'i': {
            char *strtolPtr;
            tputInterval = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0')) {
                fprintf(stderr, "option -d requires a numeric arg\n");
                Usage(argv[0]);
            }
            break;
        }

        case 'p':
            if (strcasecmp(optarg, "udp") == 0) {
                transport_type = TRANSPORT_UDP;
            } else if (strcasecmp(optarg, "dpdk") == 0) {
                transport_type = TRANSPORT_DPDK;
            } else {
                fprintf(stderr, "unknown transport '%s'\n", optarg);
                Usage(argv[0]);
            }
            break;

        case 'z':
            transport_cmdline = std::string(optarg);
            break;

        default:
            fprintf(stderr, "Unknown argument %s\n", argv[optind]);
            Usage(argv[0]);
            break;
        }
    }

    if (!configPath) {
        fprintf(stderr, "option -c is required\n");
        Usage(argv[0]);
    }
    if (host.empty()) {
        fprintf(stderr, "option -h is required\n");
        Usage(argv[0]);
    }
    if (proto == PROTO_UNKNOWN) {
        fprintf(stderr, "option -m is required\n");
        Usage(argv[0]);
    }

    // Load configuration
    std::ifstream configStream(configPath);
    if (configStream.fail()) {
        fprintf(stderr, "unable to read configuration file: %s\n", configPath);
        Usage(argv[0]);
    }
    dsnet::Configuration config(configStream);

    dsnet::Transport *transport;
    switch (transport_type) {
    case TRANSPORT_UDP:
        transport = new dsnet::UDPTransport(0, 0);
        break;
    case TRANSPORT_DPDK:
        transport =
            new dsnet::DPDKTransport(dev_port, 0, 1, 0, transport_cmdline);
        break;
    }

    Recv recv(transport, config, host);
    transport->Timer(0, [&] { recv.Start(); });
    transport->Run();

    delete transport;
}
