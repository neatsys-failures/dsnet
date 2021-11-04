d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	replica.cc client.cc)

PROTOS += $(addprefix $(d), \
	    signedunrep-proto.proto)

OBJS-signedunrep-client := $(o)client.o $(o)signedunrep-proto.o \
               $(OBJS-client) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter)

OBJS-signedunrep-replica := $(o)replica.o $(o)signedunrep-proto.o \
               $(OBJS-replica) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter) \
               $(LIB-taskqueue)

$(o)client.o $(o)replica.o: $(o)signedunrep-proto.o
