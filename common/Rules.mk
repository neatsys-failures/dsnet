d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	client.cc replica.cc log.cc pbmessage.cc taskqueue.cc signedadapter.cc runner.cc)

PROTOS += $(addprefix $(d), \
	  request.proto)

LIB-request := $(o)request.o

LIB-pbmessage := $(o)pbmessage.o

LIB-taskqueue := $(o)taskqueue.o $(LIB-message)

LIB-runner := $(o)runner.o $(LIB-latency) $(LIB-message)
$(o)runner.o: $(LIB-latency)

LIB-signedadapter := $(o)signedadapter.o $(LIB-message)

OBJS-client := $(o)client.o \
		$(LIB-message) $(LIB-configuration) $(LIB-transport) \
		$(LIB-request)

OBJS-replica := $(o)replica.o $(o)log.o \
		$(LIB-message) $(LIB-request) \
		$(LIB-configuration) $(LIB-udptransport)
