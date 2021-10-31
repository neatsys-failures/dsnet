d := $(dir $(lastword $(MAKEFILE_LIST)))

GTEST_SRCS += \
	$(d)fastpaxos-test.cc \
	$(d)nopaxos-test.cc \
	$(d)spec-test.cc \
	$(d)spec/merge-test.cc \
	$(d)vr-test.cc \
	$(d)unreplicated-test.cc

PROTOS += $(d)spec/merge-test-case.proto

TEST_BINS += \
	$(d)fastpaxos-test \
	$(d)nopaxos-test \
	$(d)spec-test \
	$(d)spec/merge-test \
	$(d)vr-test \
	$(d)unreplicated-test

$(d)fastpaxos-test: $(o)fastpaxos-test.o \
	$(OBJS-fastpaxos-replica) \
	$(OBJS-fastpaxos-client) \
	$(LIB-simtransport) \
	$(GTEST_MAIN)

$(d)vr-test: $(o)vr-test.o \
	$(OBJS-vr-replica) \
	$(OBJS-vr-client) \
	$(LIB-simtransport) \
	$(GTEST_MAIN)

$(d)nopaxos-test: $(o)nopaxos-test.o \
	$(OBJS-nopaxos-replica) \
	$(OBJS-nopaxos-client) \
	$(OBJS-nopaxos-sequencer) \
	$(OBJS-sequencer) \
	$(LIB-simtransport) \
	$(GTEST_MAIN)

$(d)unreplicated-test: $(o)unreplicated-test.o \
	$(OBJS-unreplicated-replica) \
	$(OBJS-unreplicated-client) \
	$(LIB-simtransport) \
	$(GTEST_MAIN)
$(o)unreplicated-test.o: $(OBJS-unreplicated-replica) $(OBJS-unreplicated-client)

$(d)spec-test: $(o)spec-test.o \
	$(OBJS-spec-replica) \
	$(OBJS-spec-client) \
	$(LIB-simtransport) \
	$(GTEST_MAIN)

$(d)spec/merge-test: $(o)spec/merge-test.o \
	$(o)spec/merge-test-case.o \
	$(OBJS-spec-replica) \
	$(OBJS-spec-client) \
	$(LIB-simtransport) \
	$(GTEST_MAIN)
