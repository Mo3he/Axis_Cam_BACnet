#include "bacnet_device.h"

#include <glib-unix.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "bacnet/apdu.h"
#include "bacnet/bacdef.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/object/av.h"
#include "bacnet/basic/object/bv.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/msv.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/dcc.h"
#include "bacnet/npdu.h"

#define TICK_MS 50

/* Every Object_Write_Property is NULL and WriteProperty is not a supported
 * service, so a write is rejected as an unrecognized service (docs/PICS.md). */
static object_functions_t object_table[] = {
    {OBJECT_DEVICE, NULL, Device_Count, Device_Index_To_Instance, Device_Valid_Object_Instance_Number,
     Device_Object_Name, Device_Read_Property_Local, NULL, Device_Property_Lists, DeviceGetRRInfo, NULL, NULL,
     NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {OBJECT_ANALOG_VALUE, Analog_Value_Init, Analog_Value_Count, Analog_Value_Index_To_Instance,
     Analog_Value_Valid_Instance, Analog_Value_Object_Name, Analog_Value_Read_Property, NULL,
     Analog_Value_Property_Lists, NULL, NULL, Analog_Value_Encode_Value_List, Analog_Value_Change_Of_Value,
     Analog_Value_Change_Of_Value_Clear, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {OBJECT_BINARY_VALUE, Binary_Value_Init, Binary_Value_Count, Binary_Value_Index_To_Instance,
     Binary_Value_Valid_Instance, Binary_Value_Object_Name, Binary_Value_Read_Property, NULL,
     Binary_Value_Property_Lists, NULL, NULL, Binary_Value_Encode_Value_List, Binary_Value_Change_Of_Value,
     Binary_Value_Change_Of_Value_Clear, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {OBJECT_MULTI_STATE_VALUE, Multistate_Value_Init, Multistate_Value_Count, Multistate_Value_Index_To_Instance,
     Multistate_Value_Valid_Instance, Multistate_Value_Object_Name, Multistate_Value_Read_Property, NULL,
     Multistate_Value_Property_Lists, NULL, NULL, Multistate_Value_Encode_Value_List,
     Multistate_Value_Change_Of_Value, Multistate_Value_Change_Of_Value_Clear, NULL, NULL, NULL, NULL, NULL, NULL,
     NULL},
    {MAX_BACNET_OBJECT_TYPE, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
     NULL, NULL, NULL, NULL, NULL, NULL},
};

static uint8_t rx_buffer[MAX_MPDU];
static guint socket_watch;
static guint broadcast_watch;
static guint tick_source;
static bool running;
static gint64 last_second_us;
static gint64 last_address_us;

static gboolean on_readable(gint fd, GIOCondition condition, gpointer user_data) {
    (void)fd;
    (void)condition;
    (void)user_data;
    BACNET_ADDRESS source = {0};
    uint16_t length = datalink_receive(&source, rx_buffer, MAX_MPDU, 0);
    if (length)
        npdu_handler(&source, rx_buffer, length);
    return G_SOURCE_CONTINUE;
}

static gboolean on_tick(gpointer user_data) {
    (void)user_data;
    gint64 now = g_get_monotonic_time();

    tsm_timer_milliseconds(TICK_MS);
    if (now - last_second_us >= G_USEC_PER_SEC) {
        uint32_t seconds = (uint32_t)((now - last_second_us) / G_USEC_PER_SEC);
        last_second_us += (gint64)seconds * G_USEC_PER_SEC;
        dcc_timer_seconds(seconds);
        handler_cov_timer_seconds(seconds);
    }
    if (now - last_address_us >= 60 * G_USEC_PER_SEC) {
        address_cache_timer((uint16_t)((now - last_address_us) / G_USEC_PER_SEC));
        last_address_us = now;
    }
    handler_cov_task();
    return G_SOURCE_CONTINUE;
}

static void register_services(void) {
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
}

/* The library ignores values longer than its buffers, so truncate first. */
static void set_device_text(bool (*setter)(const char *, size_t), const char *value, size_t max_length) {
    size_t length = value ? strlen(value) : 0;
    setter(value ? value : "", length > max_length ? max_length : length);
}

static bool create_object(const ObjectConfig *object) {
    uint32_t instance = object->instance;

    switch (object->kind) {
    case OBJ_ANALOG_VALUE:
        if (Analog_Value_Create(instance) != instance)
            return false;
        Analog_Value_Name_Duplicate(instance, object->name);
        if (object->description[0])
            Analog_Value_Description_Duplicate(instance, object->description);
        Analog_Value_Units_Set(instance, (BACNET_ENGINEERING_UNITS)object->units);
        Analog_Value_COV_Increment_Set(instance, (float)object->cov_increment);
        Analog_Value_Present_Value_Set(instance, 0.0f, BACNET_MAX_PRIORITY);
        break;
    case OBJ_BINARY_VALUE:
        if (Binary_Value_Create(instance) != instance)
            return false;
        Binary_Value_Name_Duplicate(instance, object->name);
        if (object->description[0])
            Binary_Value_Description_Duplicate(instance, object->description);
        /* These two keep the pointer; the strings live in the objects config. */
        if (object->active_text[0])
            Binary_Value_Active_Text_Set(instance, object->active_text);
        if (object->inactive_text[0])
            Binary_Value_Inactive_Text_Set(instance, object->inactive_text);
        Binary_Value_Present_Value_Set(instance, object->invert ? BINARY_ACTIVE : BINARY_INACTIVE);
        break;
    case OBJ_MULTISTATE_VALUE: {
        /* NUL-separated state names ending with an empty name. */
        char list[MAX_STATES * (STATE_TEXT_MAX_LEN + 1) + 1];
        size_t used = 0;
        if (Multistate_Value_Create(instance) != instance)
            return false;
        Multistate_Value_Name_Duplicate(instance, object->name);
        if (object->description[0])
            Multistate_Value_Description_Duplicate(instance, object->description);
        for (int i = 0; i < object->state_count; i++) {
            size_t length = strlen(object->states[i]);
            memcpy(list + used, object->states[i], length + 1);
            used += length + 1;
        }
        list[used] = '\0';
        Multistate_Value_State_Text_List_Set(instance, list);
        Multistate_Value_Present_Value_Set(instance, 1);
        break;
    }
    }
    return true;
}

bool bacnet_device_start(const DeviceSettings *settings, const ObjectsConfig *objects, char *error,
                         size_t error_size) {
    if (running)
        bacnet_device_stop();

    address_init();
    Device_Set_Object_Instance_Number(settings->instance);
    Device_Init(object_table);
    handler_cov_init();
    Device_Object_Name_ANSI_Init(settings->name);
    set_device_text(Device_Set_Model_Name, settings->model, MAX_DEV_MOD_LEN);
    set_device_text(Device_Set_Firmware_Revision, settings->firmware, MAX_DEV_VER_LEN);
    set_device_text(Device_Set_Application_Software_Version, settings->app_version, MAX_DEV_VER_LEN);
    set_device_text(Device_Set_Location, settings->location, MAX_DEV_LOC_LEN);
    set_device_text(Device_Set_Description, settings->description, MAX_DEV_DESC_LEN);
    set_device_text(Device_Serial_Number_Set, settings->serial, MAX_DEV_DESC_LEN);

    for (int i = 0; i < objects->count; i++) {
        if (!create_object(&objects->objects[i])) {
            snprintf(error, error_size, "could not create %s %u", object_kind_name(objects->objects[i].kind),
                     objects->objects[i].instance);
            bacnet_device_stop();
            return false;
        }
    }

    /* Creating objects bumps the revision; clients compare it across restarts,
     * so use the persisted value that changes only with the configuration. */
    Device_Set_Database_Revision(settings->database_revision);

    bip_set_port(settings->port);
    bip_set_broadcast_port(settings->port);
    if (!datalink_init(NULL)) {
        snprintf(error, error_size, "could not open UDP port %u (no IPv4 address, or port in use)", settings->port);
        Analog_Value_Cleanup();
        Binary_Value_Cleanup();
        Multistate_Value_Cleanup();
        return false;
    }
    register_services();

    socket_watch = g_unix_fd_add(bip_get_socket(), G_IO_IN, on_readable, NULL);
    if (bip_get_broadcast_socket() >= 0 && bip_get_broadcast_socket() != bip_get_socket())
        broadcast_watch = g_unix_fd_add(bip_get_broadcast_socket(), G_IO_IN, on_readable, NULL);
    last_second_us = last_address_us = g_get_monotonic_time();
    tick_source = g_timeout_add(TICK_MS, on_tick, NULL);
    running = true;

    Send_I_Am(&Handler_Transmit_Buffer[0]);
    syslog(LOG_INFO, "BACnet device %u '%s' on UDP %u with %d objects", settings->instance, settings->name,
           settings->port, objects->count);
    return true;
}

void bacnet_device_stop(void) {
    if (socket_watch)
        g_source_remove(socket_watch);
    if (broadcast_watch)
        g_source_remove(broadcast_watch);
    if (tick_source)
        g_source_remove(tick_source);
    socket_watch = broadcast_watch = tick_source = 0;
    if (running)
        datalink_cleanup();
    running = false;
    handler_cov_init();
    Analog_Value_Cleanup();
    Binary_Value_Cleanup();
    Multistate_Value_Cleanup();
}

bool bacnet_device_running(void) {
    return running;
}

void bacnet_device_set_analog(const ObjectConfig *object, double value) {
    if (running)
        Analog_Value_Present_Value_Set(object->instance, (float)value, BACNET_MAX_PRIORITY);
}

void bacnet_device_set_binary(const ObjectConfig *object, bool active) {
    if (running)
        Binary_Value_Present_Value_Set(object->instance, active ? BINARY_ACTIVE : BINARY_INACTIVE);
}

void bacnet_device_set_state(const ObjectConfig *object, uint32_t state) {
    if (running)
        Multistate_Value_Present_Value_Set(object->instance, state);
}

void bacnet_device_set_health(const ObjectConfig *object, ObjectHealth health) {
    BACNET_RELIABILITY reliability = health == OBJECT_CONFIG_ERROR ? RELIABILITY_CONFIGURATION_ERROR
                                     : health == OBJECT_NO_DATA    ? RELIABILITY_NO_SENSOR
                                                                   : RELIABILITY_NO_FAULT_DETECTED;
    if (!running)
        return;
    switch (object->kind) {
    case OBJ_ANALOG_VALUE:
        Analog_Value_Reliability_Set(object->instance, reliability);
        break;
    case OBJ_BINARY_VALUE:
        Binary_Value_Reliability_Set(object->instance, reliability);
        break;
    case OBJ_MULTISTATE_VALUE:
        Multistate_Value_Reliability_Set(object->instance, reliability);
        break;
    }
}

void bacnet_device_value_text(const ObjectConfig *object, char *out, size_t out_size) {
    if (!running) {
        snprintf(out, out_size, "%s", "");
        return;
    }
    switch (object->kind) {
    case OBJ_ANALOG_VALUE:
        snprintf(out, out_size, "%g", (double)Analog_Value_Present_Value(object->instance));
        break;
    case OBJ_BINARY_VALUE:
        snprintf(out, out_size, "%s",
                 Binary_Value_Present_Value(object->instance) == BINARY_ACTIVE ? "active" : "inactive");
        break;
    case OBJ_MULTISTATE_VALUE: {
        uint32_t state = Multistate_Value_Present_Value(object->instance);
        if (state >= 1 && state <= (uint32_t)object->state_count)
            snprintf(out, out_size, "%u (%s)", state, object->states[state - 1]);
        else
            snprintf(out, out_size, "%u", state);
        break;
    }
    }
}

void bacnet_device_address(char *out, size_t out_size) {
    BACNET_IP_ADDRESS address;
    if (!running || !bip_get_addr(&address)) {
        snprintf(out, out_size, "%s", "");
        return;
    }
    snprintf(out, out_size, "%u.%u.%u.%u", address.address[0], address.address[1], address.address[2],
             address.address[3]);
}
