d := $(dir $(lastword $(MAKEFILE_LIST)))
SRCS += $(addprefix $(d), replica.cc)
PROTOS += $(d)minbft-proto.proto
OBJS-minbft-client := $(o)minbft-proto.o $(OBJS-client) $(LIB-signature) $(LIB-pbmessage)
OBJS-minbft-replica := $(o)replica.o $(o)minbft-proto.o $(OBJS-replica) $(LIB-signature) $(LIB-pbmessage)