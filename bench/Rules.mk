d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	client.cc benchmark.cc replica.cc)

OBJS-benchmark := $(o)benchmark.o \
                  $(LIB-message) $(LIB-latency)
$(o)benchmark.o: $(LIB-latency)

$(d)client: $(o)client.o $(LIB-udptransport) $(LIB-dpdktransport)
$(d)client $(o)client.o: $(OBJS-benchmark)
$(d)client $(o)client.o: $(OBJS-vr-client) $(OBJS-fastpaxos-client) $(OBJS-unreplicated-client) $(OBJS-nopaxos-client)
$(d)client $(o)client.o: $(OBJS-spec-client)

$(d)replica: $(o)replica.o $(LIB-udptransport) $(LIB-dpdktransport)
$(d)replica $(o)replica.o: $(OBJS-vr-replica) $(OBJS-fastpaxos-replica) $(OBJS-unreplicated-replica) $(OBJS-nopaxos-replica)
$(d)replica $(o)replica.o: $(OBJS-spec-replica)

BINS += $(d)client $(d)replica
