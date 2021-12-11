d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	replica.cc client.cc adapter.cc)

PROTOS += $(addprefix $(d), \
	    message.proto)

OBJS-tombft-client := $(o)client.o  $(o)adapter.o $(o)message.o \
               $(o)halfsiphash.o \
               $(OBJS-client) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter)

OBJS-tombft-replica := $(o)replica.o $(o)adapter.o $(o)message.o \
               $(o)halfsiphash.o \
               $(OBJS-replica) $(LIB-message) \
               $(LIB-configuration) $(LIB-pbmessage) $(LIB-signedadapter) \
               $(LIB-runner)

$(o)client.o $(o)replica.o: $(o)message.o

define compilec
	@mkdir -p $(dir $@)
	$(call trace,$(1),$<,\
	  $(CC) -iquote. $(CFLAGS) $(CFLAGS-$<) $(2) $(DEPFLAGS) -E $<)
	$(Q)$(CC) -iquote. $(CFLAGS) $(CFLAGS-$<) $(2) -c -o $@ $<
endef

$(o)halfsiphash.o: $(d)halfsiphash.c
	$(call compilec,CC,)
