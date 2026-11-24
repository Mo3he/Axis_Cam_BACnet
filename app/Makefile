PROG = BACnet_Server
SRCS = main.c bacnet_device.c bindings.c objects_config.c cJSON.c
PKGS = axparameter axevent glib-2.0 gio-2.0 libcurl

BACNET_STACK ?= /opt/bacnet-stack
# Must match the defines the library was built with (Dockerfile).
BACNET_DEFINES = -DBACDL_BIP=1 -DBBMD_ENABLED=0 -DWEAK_FUNC=

CFLAGS += -Wall -Wextra -Werror $(shell pkg-config --cflags $(PKGS))
CFLAGS += -I$(BACNET_STACK)/src $(BACNET_DEFINES)
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += -Wl,-z,noexecstack -Wl,--gc-sections
LDLIBS += $(BACNET_STACK)/apps/lib/libbacnet.a $(shell pkg-config --libs $(PKGS)) -lm

# The library leaves the Device object to the application. Use its
# configurable implementation with only the object types this app serves, so
# no other object type is linked in. Built unmodified, without our -Werror.
# BACnet vendor identifier and name in the Device object. 260 is
# bacnet-stack's default; a product needs its own ID from ASHRAE.
BACNET_VENDOR_ID ?= 260
BACNET_VENDOR_NAME ?= BACnet Stack at SourceForge
BACNET_DEVICE_DEFINES = -DBACNET_VENDOR_ID=$(BACNET_VENDOR_ID) \
	-DBACNET_VENDOR_NAME='"$(BACNET_VENDOR_NAME)"' \
	-DCONFIG_BACNET_BASIC_OBJECT_ANALOG_VALUE \
	-DCONFIG_BACNET_BASIC_OBJECT_BINARY_VALUE \
	-DCONFIG_BACNET_BASIC_OBJECT_MULTISTATE_VALUE \
	-DCONFIG_BACNET_BASIC_COV_SUBSCRIPTIONS_SIZE=128
BACNET_OBJECT_DIR = $(BACNET_STACK)/src/bacnet/basic/object
BACNET_OBJECTS = bacnet_device_object.o bacnet_av.o bacnet_bv.o bacnet_msv.o bacnet_netport.o
BACNET_OBJECT_CFLAGS = $(filter-out -Werror -Wall -Wextra,$(CFLAGS)) $(BACNET_DEVICE_DEFINES)

all: $(PROG)

bacnet_device_object.o: $(BACNET_STACK)/src/bacnet/basic/server/bacnet_device.c
	$(CC) $(BACNET_OBJECT_CFLAGS) -c -o $@ $<

bacnet_av.o: $(BACNET_OBJECT_DIR)/av.c
	$(CC) $(BACNET_OBJECT_CFLAGS) -c -o $@ $<

bacnet_bv.o: $(BACNET_OBJECT_DIR)/bv.c
	$(CC) $(BACNET_OBJECT_CFLAGS) -c -o $@ $<

bacnet_msv.o: $(BACNET_OBJECT_DIR)/msv.c
	$(CC) $(BACNET_OBJECT_CFLAGS) -c -o $@ $<

bacnet_netport.o: $(BACNET_OBJECT_DIR)/netport.c
	$(CC) $(BACNET_OBJECT_CFLAGS) -c -o $@ $<

$(PROG): $(SRCS) $(BACNET_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(PROG) $(BACNET_OBJECTS)
