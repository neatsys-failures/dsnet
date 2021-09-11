d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	replica.cc client.cc)

PROTOS += $(addprefix $(d), \
	    unreplicated-sig-proto.proto)

OBJS-unreplicated-sig-client := $(o)client.o $(o)unreplicated-sig-proto.o \
               $(OBJS-client) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage)

OBJS-unreplicated-sig-replica := $(o)replica.o $(o)unreplicated-sig-proto.o \
               $(OBJS-replica) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage)
