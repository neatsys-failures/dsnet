d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(d)replica.cc $(d)client.cc $(d)adapter.cc
PROTOS += $(d)message.proto

OBJS-minbft-client := $(o)client.o $(o)message.o $(o)adapter.o $(OBJS-client) \
	$(LIB-message) $(LIB-pbmessage)
OBJS-minbft-replica := $(o)replica.o $(o)message.o $(o)adapter.o $(OBJS-replica) \
	$(LIB-message) $(LIB-pbmessage) $(LIB-runner)

# protobuf dependency
$(o)client.o $(o)replica.o: $(o)message.o