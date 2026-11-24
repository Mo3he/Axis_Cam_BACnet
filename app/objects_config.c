#include "objects_config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct {
    ObjectKind kind;
    const char *name;
} kind_names[] = {
    {OBJ_ANALOG_VALUE, "analog-value"},
    {OBJ_BINARY_VALUE, "binary-value"},
    {OBJ_MULTISTATE_VALUE, "multistate-value"},
};

const char *object_kind_name(ObjectKind kind) {
    for (size_t i = 0; i < sizeof(kind_names) / sizeof(kind_names[0]); i++) {
        if (kind_names[i].kind == kind)
            return kind_names[i].name;
    }
    return "unknown";
}

static bool kind_from_name(const char *name, ObjectKind *kind) {
    for (size_t i = 0; name && i < sizeof(kind_names) / sizeof(kind_names[0]); i++) {
        if (strcmp(kind_names[i].name, name) == 0) {
            *kind = kind_names[i].kind;
            return true;
        }
    }
    return false;
}

static bool fail(char *error, size_t error_size, int index, const char *field, const char *reason) {
    if (index >= 0)
        snprintf(error, error_size, "objects[%d].%s: %s", index, field, reason);
    else
        snprintf(error, error_size, "%s: %s", field, reason);
    return false;
}

/* Copies an optional string field; missing means empty. */
static bool copy_string(const cJSON *object, const char *key, char *out, size_t out_size, bool required,
                        char *error, size_t error_size, int index) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    out[0] = '\0';
    if (!item || cJSON_IsNull(item)) {
        if (required)
            return fail(error, error_size, index, key, "is required");
        return true;
    }
    if (!cJSON_IsString(item))
        return fail(error, error_size, index, key, "must be a string");
    if (strlen(item->valuestring) >= out_size) {
        char reason[64];
        snprintf(reason, sizeof(reason), "is longer than %zu characters", out_size - 1);
        return fail(error, error_size, index, key, reason);
    }
    if (required && item->valuestring[0] == '\0')
        return fail(error, error_size, index, key, "must not be empty");
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

static bool valid_identifier(const char *text) {
    if (!*text)
        return false;
    for (const char *c = text; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.')
            return false;
    }
    return true;
}

static bool parse_topic(const cJSON *topic, EventBinding *binding, char *error, size_t error_size, int index) {
    if (!cJSON_IsArray(topic) || cJSON_GetArraySize(topic) == 0)
        return fail(error, error_size, index, "event.topic", "must be a non-empty array");
    if (cJSON_GetArraySize(topic) > MAX_TOPIC_LEVELS)
        return fail(error, error_size, index, "event.topic", "has more than 6 levels");

    int level = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, topic) {
        char field[32];
        snprintf(field, sizeof(field), "event.topic[%d]", level);
        if (!cJSON_IsString(item))
            return fail(error, error_size, index, field, "must be a string");
        const char *colon = strchr(item->valuestring, ':');
        if (!colon)
            return fail(error, error_size, index, field, "must be namespace:name, e.g. tns1:Device");
        size_t ns_len = (size_t)(colon - item->valuestring);
        TopicLevel *out = &binding->topic[level];
        if (ns_len != 4 && ns_len != 7)
            return fail(error, error_size, index, field, "namespace must be tns1 or tnsaxis");
        memcpy(out->ns, item->valuestring, ns_len);
        out->ns[ns_len] = '\0';
        if (strcmp(out->ns, "tns1") != 0 && strcmp(out->ns, "tnsaxis") != 0)
            return fail(error, error_size, index, field, "namespace must be tns1 or tnsaxis");
        if (strlen(colon + 1) > TOPIC_NAME_MAX_LEN || !valid_identifier(colon + 1))
            return fail(error, error_size, index, field, "has an invalid topic name");
        snprintf(out->name, sizeof(out->name), "%s", colon + 1);
        level++;
    }
    binding->topic_count = level;
    return true;
}

static bool parse_binding(const cJSON *object, EventBinding *binding, char *error, size_t error_size, int index) {
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(object, "event");
    if (!cJSON_IsObject(event))
        return fail(error, error_size, index, "event", "is required");
    if (!parse_topic(cJSON_GetObjectItemCaseSensitive(event, "topic"), binding, error, error_size, index))
        return false;
    if (!copy_string(event, "data_key", binding->data_key, sizeof(binding->data_key), true, error, error_size,
                     index)) {
        /* copy_string reports the field as "data_key"; name its full path. */
        char reason[96];
        const char *colon = strstr(error, ": ");
        snprintf(reason, sizeof(reason), "%s", colon ? colon + 2 : "is invalid");
        return fail(error, error_size, index, "event.data_key", reason);
    }
    if (!valid_identifier(binding->data_key))
        return fail(error, error_size, index, "event.data_key", "has invalid characters");
    if (!copy_string(event, "source_key", binding->source_key, sizeof(binding->source_key), false, error,
                     error_size, index) ||
        !copy_string(event, "source_value", binding->source_value, sizeof(binding->source_value), false, error,
                     error_size, index)) {
        char reason[96];
        const char *colon = strstr(error, ": ");
        const char *field = strstr(error, "].source_key") ? "event.source_key" : "event.source_value";
        snprintf(reason, sizeof(reason), "%s", colon ? colon + 2 : "is invalid");
        return fail(error, error_size, index, field, reason);
    }
    if (binding->source_key[0] && !valid_identifier(binding->source_key))
        return fail(error, error_size, index, "event.source_key", "has invalid characters");
    if (binding->source_value[0] && !binding->source_key[0])
        return fail(error, error_size, index, "event.source_key", "is required when source_value is set");
    return true;
}

static bool parse_object(const cJSON *json, ObjectConfig *object, char *error, size_t error_size, int index) {
    memset(object, 0, sizeof(*object));
    object->units = DEFAULT_UNITS;

    if (!cJSON_IsObject(json))
        return fail(error, error_size, index, "", "must be an object");

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    if (!cJSON_IsString(type) || !kind_from_name(type->valuestring, &object->kind))
        return fail(error, error_size, index, "type", "must be analog-value, binary-value or multistate-value");

    const cJSON *instance = cJSON_GetObjectItemCaseSensitive(json, "instance");
    if (!cJSON_IsNumber(instance) || instance->valuedouble < 0 || instance->valuedouble > MAX_OBJECT_INSTANCE ||
        floor(instance->valuedouble) != instance->valuedouble)
        return fail(error, error_size, index, "instance", "must be a whole number from 0 to 4194302");
    object->instance = (uint32_t)instance->valuedouble;

    if (!copy_string(json, "name", object->name, sizeof(object->name), true, error, error_size, index) ||
        !copy_string(json, "description", object->description, sizeof(object->description), false, error,
                     error_size, index))
        return false;

    if (object->kind == OBJ_ANALOG_VALUE) {
        const cJSON *units = cJSON_GetObjectItemCaseSensitive(json, "units");
        if (units && !cJSON_IsNull(units)) {
            if (!cJSON_IsNumber(units) || units->valuedouble < 0 || units->valuedouble > 65535 ||
                floor(units->valuedouble) != units->valuedouble)
                return fail(error, error_size, index, "units", "must be a BACnet engineering units number");
            object->units = (uint16_t)units->valuedouble;
        }
        const cJSON *cov = cJSON_GetObjectItemCaseSensitive(json, "cov_increment");
        if (cov && !cJSON_IsNull(cov)) {
            if (!cJSON_IsNumber(cov) || cov->valuedouble < 0)
                return fail(error, error_size, index, "cov_increment", "must be zero or a positive number");
            object->cov_increment = cov->valuedouble;
        }
    } else if (object->kind == OBJ_BINARY_VALUE) {
        const cJSON *invert = cJSON_GetObjectItemCaseSensitive(json, "invert");
        if (invert && !cJSON_IsNull(invert) && !cJSON_IsBool(invert))
            return fail(error, error_size, index, "invert", "must be true or false");
        object->invert = cJSON_IsTrue(invert);
        if (!copy_string(json, "active_text", object->active_text, sizeof(object->active_text), false, error,
                         error_size, index) ||
            !copy_string(json, "inactive_text", object->inactive_text, sizeof(object->inactive_text), false,
                         error, error_size, index))
            return false;
    } else {
        const cJSON *states = cJSON_GetObjectItemCaseSensitive(json, "states");
        if (!cJSON_IsArray(states) || cJSON_GetArraySize(states) < MIN_STATES ||
            cJSON_GetArraySize(states) > MAX_STATES)
            return fail(error, error_size, index, "states", "must be a list of 2 to 16 state names");
        const cJSON *state;
        cJSON_ArrayForEach(state, states) {
            char field[24];
            snprintf(field, sizeof(field), "states[%d]", object->state_count);
            if (!cJSON_IsString(state) || state->valuestring[0] == '\0')
                return fail(error, error_size, index, field, "must be a non-empty string");
            if (strlen(state->valuestring) > STATE_TEXT_MAX_LEN)
                return fail(error, error_size, index, field, "is longer than 32 characters");
            for (int i = 0; i < object->state_count; i++) {
                if (strcasecmp(object->states[i], state->valuestring) == 0)
                    return fail(error, error_size, index, field, "duplicates another state");
            }
            snprintf(object->states[object->state_count++], STATE_TEXT_MAX_LEN + 1, "%s", state->valuestring);
        }
    }

    return parse_binding(json, &object->binding, error, error_size, index);
}

bool objects_config_parse(const cJSON *json, ObjectsConfig *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    const cJSON *objects = cJSON_IsObject(json) ? cJSON_GetObjectItemCaseSensitive(json, "objects") : NULL;
    if (!cJSON_IsArray(objects))
        return fail(error, error_size, -1, "objects", "must be an array");
    if (cJSON_GetArraySize(objects) > MAX_OBJECTS)
        return fail(error, error_size, -1, "objects", "has more than 64 entries");

    const cJSON *item;
    cJSON_ArrayForEach(item, objects) {
        int index = out->count;
        ObjectConfig *object = &out->objects[index];
        if (!parse_object(item, object, error, error_size, index))
            return false;
        for (int i = 0; i < index; i++) {
            if (out->objects[i].kind == object->kind && out->objects[i].instance == object->instance)
                return fail(error, error_size, index, "instance", "is already used by another object of this type");
            if (strcmp(out->objects[i].name, object->name) == 0)
                return fail(error, error_size, index, "name", "is already used by another object");
        }
        out->count++;
    }
    return true;
}

cJSON *objects_config_to_json(const ObjectsConfig *config) {
    cJSON *root = cJSON_CreateObject();
    cJSON *objects = cJSON_AddArrayToObject(root, "objects");

    for (int i = 0; i < config->count; i++) {
        const ObjectConfig *object = &config->objects[i];
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "type", object_kind_name(object->kind));
        cJSON_AddNumberToObject(json, "instance", object->instance);
        cJSON_AddStringToObject(json, "name", object->name);
        cJSON_AddStringToObject(json, "description", object->description);
        if (object->kind == OBJ_ANALOG_VALUE) {
            cJSON_AddNumberToObject(json, "units", object->units);
            cJSON_AddNumberToObject(json, "cov_increment", object->cov_increment);
        } else if (object->kind == OBJ_BINARY_VALUE) {
            cJSON_AddBoolToObject(json, "invert", object->invert);
            cJSON_AddStringToObject(json, "active_text", object->active_text);
            cJSON_AddStringToObject(json, "inactive_text", object->inactive_text);
        } else {
            cJSON *states = cJSON_AddArrayToObject(json, "states");
            for (int s = 0; s < object->state_count; s++)
                cJSON_AddItemToArray(states, cJSON_CreateString(object->states[s]));
        }

        cJSON *event = cJSON_AddObjectToObject(json, "event");
        cJSON *topic = cJSON_AddArrayToObject(event, "topic");
        for (int t = 0; t < object->binding.topic_count; t++) {
            char level[sizeof(object->binding.topic[t].ns) + TOPIC_NAME_MAX_LEN + 2];
            snprintf(level, sizeof(level), "%s:%s", object->binding.topic[t].ns, object->binding.topic[t].name);
            cJSON_AddItemToArray(topic, cJSON_CreateString(level));
        }
        cJSON_AddStringToObject(event, "source_key", object->binding.source_key);
        cJSON_AddStringToObject(event, "source_value", object->binding.source_value);
        cJSON_AddStringToObject(event, "data_key", object->binding.data_key);
        cJSON_AddItemToArray(objects, json);
    }
    return root;
}

void binding_topic_path(const EventBinding *binding, char *out, size_t out_size) {
    size_t used = 0;
    out[0] = '\0';
    for (int t = 0; t < binding->topic_count && used < out_size; t++) {
        int written = snprintf(out + used, out_size - used, "%s%s:%s", t ? "/" : "", binding->topic[t].ns,
                               binding->topic[t].name);
        if (written < 0)
            break;
        used += (size_t)written;
    }
}

void event_value_to_string(const EventValue *value, char *out, size_t out_size) {
    switch (value->type) {
    case EV_BOOL:
        snprintf(out, out_size, "%s", value->b ? "1" : "0");
        break;
    case EV_INT:
        snprintf(out, out_size, "%lld", value->i);
        break;
    case EV_DOUBLE:
        snprintf(out, out_size, "%g", value->d);
        break;
    default:
        snprintf(out, out_size, "%s", value->s);
        break;
    }
}

static bool parse_number(const char *text, double *out) {
    char *end = NULL;
    while (isspace((unsigned char)*text))
        text++;
    if (!*text)
        return false;
    double parsed = strtod(text, &end);
    while (end && isspace((unsigned char)*end))
        end++;
    if (!end || *end || !isfinite(parsed))
        return false;
    *out = parsed;
    return true;
}

bool event_value_to_analog(const EventValue *value, double *out) {
    switch (value->type) {
    case EV_BOOL:
        *out = value->b ? 1.0 : 0.0;
        return true;
    case EV_INT:
        *out = (double)value->i;
        return true;
    case EV_DOUBLE:
        if (!isfinite(value->d))
            return false;
        *out = value->d;
        return true;
    default:
        return parse_number(value->s, out);
    }
}

bool event_value_to_binary(const EventValue *value, bool invert, bool *active) {
    bool result = false;
    switch (value->type) {
    case EV_BOOL:
        result = value->b;
        break;
    case EV_INT:
        result = value->i != 0;
        break;
    case EV_DOUBLE:
        result = value->d != 0.0;
        break;
    default: {
        static const char *const on[] = {"1", "true", "yes", "on", "active", "high"};
        static const char *const off[] = {"0", "false", "no", "off", "inactive", "low"};
        bool matched = false;
        for (size_t i = 0; i < sizeof(on) / sizeof(on[0]) && !matched; i++) {
            if (strcasecmp(value->s, on[i]) == 0) {
                result = true;
                matched = true;
            } else if (strcasecmp(value->s, off[i]) == 0) {
                result = false;
                matched = true;
            }
        }
        if (!matched)
            return false;
        break;
    }
    }
    *active = invert ? !result : result;
    return true;
}

bool event_value_to_state(const ObjectConfig *object, const EventValue *value, uint32_t *state) {
    if (value->type == EV_STRING) {
        for (int i = 0; i < object->state_count; i++) {
            if (strcasecmp(object->states[i], value->s) == 0) {
                *state = (uint32_t)i + 1;
                return true;
            }
        }
    }
    double number;
    if (!event_value_to_analog(value, &number) || floor(number) != number || number < 1 ||
        number > object->state_count)
        return false;
    *state = (uint32_t)number;
    return true;
}

bool binding_aoa_scenario(const EventBinding *binding, int *scenario) {
    int device, id, end = 0;
    if (binding->topic_count != 3 || binding->source_key[0])
        return false;
    if (strcmp(binding->topic[0].name, "CameraApplicationPlatform") != 0 ||
        strcmp(binding->topic[1].name, "ObjectAnalytics") != 0)
        return false;
    if (sscanf(binding->topic[2].name, "Device%dScenario%d%n", &device, &id, &end) != 2 ||
        binding->topic[2].name[end] != '\0' || id < 1)
        return false;
    *scenario = id;
    return true;
}

bool aoa_occupancy_value(const cJSON *response, const char *key, EventValue *out) {
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(data, key);
    if (!cJSON_IsNumber(count) || count->valuedouble < 0 || count->valuedouble != floor(count->valuedouble))
        return false;
    memset(out, 0, sizeof(*out));
    out->type = EV_INT;
    out->i = (long long)count->valuedouble;
    return true;
}
