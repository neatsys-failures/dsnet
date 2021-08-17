d := $(dir $(lastword $(MAKEFILE_LIST)))
PROTOS += $(d)minbft-proto.proto
OBJS-minbft-client = $(OBJS-client) $(LIB-message) $(LIB-configuration)
