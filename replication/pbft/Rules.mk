d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(d)replica.cc $(d)client.cc
PROTOS += $(d)message.proto

# protobuf dependency
$(o)replica.o $(o)client.o: $(o)message.o

OBJS-pbft-client := \
	$(o)client.o $(o)message.o $(OBJS-client) \
	$(LIB-message) $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter)
OBJS-pbft-replica := \
	$(o)replica.o $(o)message.o $(OBJS-replica) \
	$(LIB-message) $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter) \
	$(LIB-runner) .obj/sequencer/sequencer.o  # hack for reusing BufferMessage
