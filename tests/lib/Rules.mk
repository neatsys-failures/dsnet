d := $(dir $(lastword $(MAKEFILE_LIST)))

#
# gtest-based tests
#
# sgdxbc: where is workertasks-test.cc?
GTEST_SRCS += $(addprefix $(d), \
			  configuration-test.cc \
			  simtransport-test.cc \
			  taskqueue-test.cc \
			  signedadapter-test.cc \
			  runner-test.cc)

PROTOS += $(d)simtransport-testmessage.proto

$(d)configuration-test: $(o)configuration-test.o $(LIB-configuration) $(GTEST_MAIN)

TEST_BINS += $(d)configuration-test

$(d)simtransport-test: $(o)simtransport-test.o $(LIB-simtransport) $(LIB-pbmessage) $(o)simtransport-testmessage.o $(GTEST_MAIN)

TEST_BINS += $(d)simtransport-test

$(d)taskqueue-test: $(o)taskqueue-test.o $(LIB-taskqueue) $(GTEST_MAIN)

TEST_BINS += $(d)taskqueue-test

$(d)signedadapter-test: $(o)signedadapter-test.o $(LIB-signedadapter) $(GTEST_MAIN)

TEST_BINS += $(d)signedadapter-test

$(d)runner-test: $(o)runner-test.o $(LIB-runner) $(GTEST_MAIN)

TEST_BINS += $(d)runner-test
