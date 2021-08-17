d := $(dir $(lastword $(MAKEFILE_LIST)))
GTEST_SRCS += $(d)minbft-test.cc
$(d)minbft-test: $(o)minbft-test.o $(OBJS-minbft-client) $(GTEST_MAIN)
TEST_BINS += $(d)minbft-test