/*
 * BACnet object declarations: parsing, validation and the conversion of
 * camera event values to BACnet present values. No BACnet or AXIS SDK
 * dependencies, so it is unit-tested on the host (tests/).
 */
#ifndef OBJECTS_CONFIG_H
#define OBJECTS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define MAX_OBJECTS 64
#define MAX_TOPIC_LEVELS 6
#define MAX_STATES 16
#define MIN_STATES 2
#define NAME_MAX_LEN 64
#define DESCRIPTION_MAX_LEN 128
#define STATE_TEXT_MAX_LEN 32
#define TOPIC_NAME_MAX_LEN 64
#define KEY_MAX_LEN 64
#define VALUE_MAX_LEN 64
#define MAX_OBJECT_INSTANCE 4194302u
#define DEFAULT_UNITS 95 /* BACnet no-units */

typedef enum { OBJ_ANALOG_VALUE, OBJ_BINARY_VALUE, OBJ_MULTISTATE_VALUE } ObjectKind;

typedef struct {
    char ns[16]; /* "tns1" or "tnsaxis" */
    char name[TOPIC_NAME_MAX_LEN + 1];
} TopicLevel;

typedef struct {
    TopicLevel topic[MAX_TOPIC_LEVELS];
    int topic_count;
    char source_key[KEY_MAX_LEN + 1]; /* optional filter, e.g. "port" */
    char source_value[VALUE_MAX_LEN + 1];
    char data_key[KEY_MAX_LEN + 1]; /* event field that carries the value */
} EventBinding;

typedef struct {
    ObjectKind kind;
    uint32_t instance;
    char name[NAME_MAX_LEN + 1];
    char description[DESCRIPTION_MAX_LEN + 1];
    uint16_t units;       /* analog only */
    double cov_increment; /* analog only */
    bool invert;          /* binary only */
    char active_text[STATE_TEXT_MAX_LEN + 1];
    char inactive_text[STATE_TEXT_MAX_LEN + 1];
    char states[MAX_STATES][STATE_TEXT_MAX_LEN + 1]; /* multistate only */
    int state_count;
    EventBinding binding;
} ObjectConfig;

typedef struct {
    ObjectConfig objects[MAX_OBJECTS];
    int count;
} ObjectsConfig;

/* Parses and validates a JSON document {"objects":[...]}. On failure returns
 * false and writes "objects[i].field: reason" to error. */
bool objects_config_parse(const cJSON *json, ObjectsConfig *out, char *error, size_t error_size);

/* Serializes to the same JSON shape objects_config_parse accepts. */
cJSON *objects_config_to_json(const ObjectsConfig *config);

const char *object_kind_name(ObjectKind kind);

/* Topic path as shown to users, e.g. "tns1:Device/tnsaxis:IO/VirtualInput". */
void binding_topic_path(const EventBinding *binding, char *out, size_t out_size);

/* An event field value, converted from the camera's typed event data. */
typedef enum { EV_BOOL, EV_INT, EV_DOUBLE, EV_STRING } EventValueType;

typedef struct {
    EventValueType type;
    bool b;
    long long i;
    double d;
    char s[VALUE_MAX_LEN + 1];
} EventValue;

/* Text form used for source filters and for the status page. */
void event_value_to_string(const EventValue *value, char *out, size_t out_size);

bool event_value_to_analog(const EventValue *value, double *out);
bool event_value_to_binary(const EventValue *value, bool invert, bool *active);
/* Matches state text (case-insensitive) or a 1-based state number. */
bool event_value_to_state(const ObjectConfig *object, const EventValue *value, uint32_t *state);

/* AXIS Object Analytics occupancy events are only sent when something
 * changes, so an object bound to one would wait for its first value. These
 * let the app read the current count once instead. True when the binding is
 * an AOA scenario event without a source filter; sets the scenario ID. */
bool binding_aoa_scenario(const EventBinding *binding, int *scenario);
/* Reads data.<key> from a getOccupancy response as an integer value. */
bool aoa_occupancy_value(const cJSON *response, const char *key, EventValue *out);

#endif
