d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	replica.cc client.cc)

PROTOS += $(addprefix $(d), \
	    message.proto)

OBJS-hotstuff-client := $(o)client.o $(o)message.o \
               $(OBJS-client) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter)

OBJS-hotstuff-replica := $(o)replica.o $(o)message.o \
               $(OBJS-replica) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter) \
               $(LIB-runner) $(LIB-latency)

$(o)client.o $(o)replica.o: $(o)message.o
