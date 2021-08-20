d := $(dir $(lastword $(MAKEFILE_LIST)))
GTEST_SRCS += $(d)minbft-test.cc
$(d)minbft-test: $(o)minbft-test.o $(OBJS-minbft-client) $(OBJS-minbft-replica) $(LIB-simtransport) $(GTEST_MAIN)
TEST_BINS += $(d)minbft-test