// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * replica/main.cc:
 *   test replica application; "null RPC" equivalent
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

#include "common/replica.h"
#include "lib/configuration.h"
#include "lib/udptransport.h"
#include "replication/fastpaxos/replica.h"
#include "replication/hotstuff/replica.h"
#include "replication/minbft/replica.h"
#include "replication/nopaxos/replica.h"
#include "replication/pbft/replica.h"
#include "replication/signedunrep/replica.h"
#include "replication/tombft/replica.h"
#include "replication/unreplicated/replica.h"
#include "replication/vr/replica.h"

#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

static void Usage(const char *progName) {
    fprintf(
        stderr,
        "usage: %s "
        "-c conf-file [-R] -i replica-index "
        "-m unreplicated|signedunrep|vr|fastpaxos|nopaxos "
        "[-b batch-size] [-d packet-drop-rate] [-r packet-reorder-rate] "
        "[-w number-worker-thread]\n",
        progName);
    exit(1);
}

int main(int argc, char **argv) {
    int index = -1;
    const char *configPath = NULL;
    double dropRate = 0.0;
    double reorderRate = 0.0;
    int batchSize = 1;
    bool recover = false;
    int n_worker_thread = 8;

    dsnet::AppReplica *nullApp = new dsnet::AppReplica();

    enum {
        PROTO_UNKNOWN,
        PROTO_UNREPLICATED,
        PROTO_SIGNEDUNREP,
        PROTO_VR,
        PROTO_FASTPAXOS,
        PROTO_SPEC,
        PROTO_NOPAXOS,
        PROTO_TOMBFT,
        PROTO_HOTSTUFF,
        PROTO_PBFT,
        PROTO_MINBFT,
        PROTO_TOMBFT_HMAC,
    } proto = PROTO_UNKNOWN;

    // Parse arguments
    int opt;
    while ((opt = getopt(argc, argv, "b:c:d:i:m:r:R:w:")) != -1) {
        switch (opt) {
        case 'b': {
            char *strtolPtr;
            batchSize = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || (batchSize < 1)) {
                fprintf(stderr, "option -b requires a numeric arg\n");
                Usage(argv[0]);
            }
            break;
        }

        case 'c':
            configPath = optarg;
            break;

        case 'd': {
            char *strtodPtr;
            dropRate = strtod(optarg, &strtodPtr);
            if ((*optarg == '\0') || (*strtodPtr != '\0') ||
                ((dropRate < 0) || (dropRate >= 1))) {
                fprintf(
                    stderr,
                    "option -d requires a numeric arg between 0 and 1\n");
                Usage(argv[0]);
            }
            break;
        }

        case 'i': {
            char *strtolPtr;
            index = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || (index < 0)) {
                fprintf(stderr, "option -i requires a numeric arg\n");
                Usage(argv[0]);
            }
            break;
        }

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

        case 'r': {
            char *strtodPtr;
            reorderRate = strtod(optarg, &strtodPtr);
            if ((*optarg == '\0') || (*strtodPtr != '\0') ||
                ((reorderRate < 0) || (reorderRate >= 1))) {
                fprintf(
                    stderr,
                    "option -r requires a numeric arg between 0 and 1\n");
                Usage(argv[0]);
            }
            break;
        }

        case 'R':
            recover = true;
            break;

        case 'w': {
            char *strtod_ptr;
            n_worker_thread = strtod(optarg, &strtod_ptr);
            if (*optarg == '\0' || *strtod_ptr != '\0' ||
                n_worker_thread <= 0) {
                Usage(argv[0]);
            }
            break;
        }

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
    if (index == -1) {
        fprintf(stderr, "option -i is required\n");
        Usage(argv[0]);
    }
    if (proto == PROTO_UNKNOWN) {
        fprintf(stderr, "option -m is required\n");
        Usage(argv[0]);
    }
    if ((proto != PROTO_VR) && (batchSize != 1)) {
        Warning("Batching enabled, but has no effect on non-VR protocols");
    }

    // Load configuration
    std::ifstream configStream(configPath);
    if (configStream.fail()) {
        fprintf(stderr, "unable to read configuration file: %s\n", configPath);
        Usage(argv[0]);
    }
    dsnet::Configuration config(configStream);

    if (index >= config.n) {
        fprintf(
            stderr,
            "replica index %d is out of bounds; "
            "only %d replicas defined\n",
            index, config.n);
        Usage(argv[0]);
    }

    dsnet::UDPTransport transport(dropRate, reorderRate, nullptr);

    dsnet::Replica *replica;
    switch (proto) {
    case PROTO_UNREPLICATED:
        replica = new dsnet::unreplicated::UnreplicatedReplica(
            config, index, !recover, &transport, nullApp);
        break;

    case PROTO_VR:
        replica = new dsnet::vr::VRReplica(
            config, index, !recover, &transport, batchSize, nullApp);
        break;

    case PROTO_FASTPAXOS:
        replica = new dsnet::fastpaxos::FastPaxosReplica(
            config, index, !recover, &transport, nullApp);
        break;

    case PROTO_NOPAXOS:
        replica = new dsnet::nopaxos::NOPaxosReplica(
            config, index, !recover, &transport, nullApp);
        break;

    case PROTO_SIGNEDUNREP:
        replica = new dsnet::signedunrep::SignedUnrepReplica(
            config, "Steve", n_worker_thread, batchSize, &transport, nullApp);
        break;

    case PROTO_TOMBFT:
        replica = new dsnet::tombft::TOMBFTReplica(
            config, index, "Steve", n_worker_thread, &transport, nullApp);
        break;

    case PROTO_TOMBFT_HMAC:
        replica = new dsnet::tombft::TOMBFTHMACReplica(
            config, index, "Steve", n_worker_thread, &transport, nullApp);
        break;

    case PROTO_HOTSTUFF:
        replica = new dsnet::hotstuff::HotStuffReplica(
            config, index, "Steve", n_worker_thread, batchSize, &transport,
            nullApp);
        break;

    case PROTO_PBFT:
        replica = new dsnet::pbft::PBFTReplica(
            config, index, "Steve", n_worker_thread, batchSize, &transport,
            nullApp);
        break;

    case PROTO_MINBFT:
        replica = new dsnet::minbft::MinBFTReplica(
            config, index, "Steve", n_worker_thread, batchSize, &transport,
            nullApp);
        break;

    default:
        NOT_REACHABLE();
    }

    transport.Run();
    delete replica;
}
