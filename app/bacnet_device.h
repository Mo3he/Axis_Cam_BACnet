/*
 * The BACnet/IP device: wraps bacnet-stack and runs it inside the GLib main
 * loop (socket watches plus a 50 ms timer), so event callbacks and BACnet
 * requests run on one thread and need no locking.
 */
#ifndef BACNET_DEVICE_H
#define BACNET_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "objects_config.h"

#define DEVICE_NAME_MAX_LEN 64
#define DEVICE_TEXT_MAX_LEN 64

typedef struct {
    uint32_t instance;
    uint16_t port;
    uint32_t database_revision;
    char name[DEVICE_NAME_MAX_LEN + 1];
    char location[DEVICE_TEXT_MAX_LEN + 1];
    char description[DEVICE_TEXT_MAX_LEN + 1];
    char model[DEVICE_TEXT_MAX_LEN + 1];
    char serial[DEVICE_TEXT_MAX_LEN + 1];
    char firmware[DEVICE_TEXT_MAX_LEN + 1];
    char app_version[DEVICE_TEXT_MAX_LEN + 1];
} DeviceSettings;

/* Creates the device and its objects and opens the UDP port. The objects
 * array must stay valid until bacnet_device_stop(): text properties point
 * into it. */
bool bacnet_device_start(const DeviceSettings *settings, const ObjectsConfig *objects, char *error,
                         size_t error_size);
void bacnet_device_stop(void);
bool bacnet_device_running(void);

void bacnet_device_set_analog(const ObjectConfig *object, double value);
void bacnet_device_set_binary(const ObjectConfig *object, bool active);
void bacnet_device_set_state(const ObjectConfig *object, uint32_t state);
typedef enum {
    OBJECT_OK,           /* NO_FAULT_DETECTED */
    OBJECT_NO_DATA,      /* NO_SENSOR: no event received since it was created */
    OBJECT_CONFIG_ERROR, /* CONFIGURATION_ERROR: the event subscription failed */
} ObjectHealth;

/* Sets Reliability; Status_Flags FAULT is set for every value but OBJECT_OK. */
void bacnet_device_set_health(const ObjectConfig *object, ObjectHealth health);

/* Present value as text for the status page. */
void bacnet_device_value_text(const ObjectConfig *object, char *out, size_t out_size);

/* IPv4 address the device answers on, as text, or "" when not running. */
void bacnet_device_address(char *out, size_t out_size);

#endif
