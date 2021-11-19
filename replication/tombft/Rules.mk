d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	replica.cc client.cc adapter.cc)

PROTOS += $(addprefix $(d), \
	    message.proto)

OBJS-tombft-client := $(o)client.o  $(o)adapter.o $(o)message.o \
               $(OBJS-client) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter)

OBJS-tombft-replica := $(o)replica.o $(o)adapter.o $(o)message.o \
               $(OBJS-replica) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter) \
               $(LIB-runner)

$(o)client.o $(o)replica.o: $(o)message.o
