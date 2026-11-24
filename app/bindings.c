#include "bindings.h"

#include <axsdk/axevent.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "bacnet_device.h"

typedef struct {
    const ObjectConfig *object;
    guint subscription;
    bool subscribed;
    char error[192];
    char last_raw[VALUE_MAX_LEN + 1];
    gint64 last_update; /* Unix time, 0 = no event yet */
    unsigned long events;
} BindingState;

static AXEventHandler *event_handler;
static BindingState states[MAX_OBJECTS];
static int state_count;

bool bindings_init(void) {
    event_handler = ax_event_handler_new();
    if (!event_handler) {
        syslog(LOG_ERR, "could not create the event handler");
        return false;
    }
    return true;
}

static bool read_field(const AXEventKeyValueSet *set, const char *key, EventValue *out) {
    AXEventValueType type;
    memset(out, 0, sizeof(*out));
    if (!ax_event_key_value_set_get_value_type(set, key, NULL, &type, NULL))
        return false;

    switch (type) {
    case AX_VALUE_TYPE_BOOL: {
        gboolean value;
        if (!ax_event_key_value_set_get_boolean(set, key, NULL, &value, NULL))
            return false;
        out->type = EV_BOOL;
        out->b = value;
        return true;
    }
    case AX_VALUE_TYPE_INT: {
        gint value;
        if (!ax_event_key_value_set_get_integer(set, key, NULL, &value, NULL))
            return false;
        out->type = EV_INT;
        out->i = value;
        return true;
    }
    case AX_VALUE_TYPE_DOUBLE: {
        gdouble value;
        if (!ax_event_key_value_set_get_double(set, key, NULL, &value, NULL))
            return false;
        out->type = EV_DOUBLE;
        out->d = value;
        return true;
    }
    case AX_VALUE_TYPE_STRING: {
        gchar *value = NULL;
        if (!ax_event_key_value_set_get_string(set, key, NULL, &value, NULL))
            return false;
        out->type = EV_STRING;
        snprintf(out->s, sizeof(out->s), "%s", value ? value : "");
        g_free(value);
        return true;
    }
    default:
        return false;
    }
}

static void apply_value(BindingState *state, const EventValue *value) {
    const ObjectConfig *object = state->object;
    bool converted = false;

    switch (object->kind) {
    case OBJ_ANALOG_VALUE: {
        double number;
        if ((converted = event_value_to_analog(value, &number)))
            bacnet_device_set_analog(object, number);
        break;
    }
    case OBJ_BINARY_VALUE: {
        bool active;
        if ((converted = event_value_to_binary(value, object->invert, &active)))
            bacnet_device_set_binary(object, active);
        break;
    }
    case OBJ_MULTISTATE_VALUE: {
        uint32_t index;
        if ((converted = event_value_to_state(object, value, &index)))
            bacnet_device_set_state(object, index);
        break;
    }
    }

    event_value_to_string(value, state->last_raw, sizeof(state->last_raw));
    if (converted) {
        state->error[0] = '\0';
    } else {
        snprintf(state->error, sizeof(state->error), "value '%s' of %s cannot be converted", state->last_raw,
                 object->binding.data_key);
    }
}

static void on_event(guint subscription, AXEvent *event, gpointer user_data) {
    (void)subscription;
    BindingState *state = user_data;
    const AXEventKeyValueSet *set = ax_event_get_key_value_set(event);
    const EventBinding *binding = &state->object->binding;
    EventValue value;

    if (binding->source_key[0]) {
        EventValue source;
        char text[VALUE_MAX_LEN + 1];
        if (!read_field(set, binding->source_key, &source))
            goto done;
        event_value_to_string(&source, text, sizeof(text));
        if (strcmp(text, binding->source_value) != 0)
            goto done;
    }

    if (read_field(set, binding->data_key, &value)) {
        state->events++;
        state->last_update = g_get_real_time() / G_USEC_PER_SEC;
        apply_value(state, &value);
    }

done:
    ax_event_free(event);
}

static bool subscribe(BindingState *state) {
    const EventBinding *binding = &state->object->binding;
    AXEventKeyValueSet *set = ax_event_key_value_set_new();
    GError *error = NULL;
    bool ok = true;

    for (int level = 0; level < binding->topic_count && ok; level++) {
        char key[16];
        snprintf(key, sizeof(key), "topic%d", level);
        ok = ax_event_key_value_set_add_key_value(set, key, binding->topic[level].ns, binding->topic[level].name,
                                                  AX_VALUE_TYPE_STRING, &error);
    }
    if (ok)
        ok = ax_event_handler_subscribe(event_handler, set, &state->subscription, on_event, state, &error);

    if (!ok) {
        char path[256];
        binding_topic_path(binding, path, sizeof(path));
        snprintf(state->error, sizeof(state->error), "subscription failed: %s",
                 error ? error->message : "unknown error");
        syslog(LOG_WARNING, "%s %u: subscribing to %s failed: %s", object_kind_name(state->object->kind),
               state->object->instance, path, error ? error->message : "unknown error");
    }
    g_clear_error(&error);
    ax_event_key_value_set_free(set);
    return ok;
}

void bindings_start(const ObjectsConfig *objects) {
    bindings_stop();
    for (int i = 0; i < objects->count; i++) {
        BindingState *state = &states[i];
        memset(state, 0, sizeof(*state));
        state->object = &objects->objects[i];
        state->subscribed = event_handler && subscribe(state);
        bacnet_device_set_fault(state->object, !state->subscribed);
    }
    state_count = objects->count;
}

void bindings_stop(void) {
    for (int i = 0; i < state_count; i++) {
        if (states[i].subscribed)
            ax_event_handler_unsubscribe(event_handler, states[i].subscription, NULL);
        states[i].subscribed = false;
    }
    state_count = 0;
}

cJSON *bindings_status(void) {
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < state_count; i++) {
        const BindingState *state = &states[i];
        cJSON *item = cJSON_CreateObject();
        char value[96];
        char path[256];

        bacnet_device_value_text(state->object, value, sizeof(value));
        binding_topic_path(&state->object->binding, path, sizeof(path));
        cJSON_AddStringToObject(item, "type", object_kind_name(state->object->kind));
        cJSON_AddNumberToObject(item, "instance", state->object->instance);
        cJSON_AddStringToObject(item, "name", state->object->name);
        cJSON_AddStringToObject(item, "present_value", value);
        cJSON_AddStringToObject(item, "topic", path);
        cJSON_AddBoolToObject(item, "subscribed", state->subscribed);
        cJSON_AddNumberToObject(item, "events", (double)state->events);
        cJSON_AddStringToObject(item, "last_event_value", state->last_raw);
        cJSON_AddNumberToObject(item, "last_event_time", (double)state->last_update);
        cJSON_AddStringToObject(item, "error", state->error);
        cJSON_AddItemToArray(list, item);
    }
    return list;
}

void bindings_cleanup(void) {
    bindings_stop();
    if (event_handler)
        ax_event_handler_free(event_handler);
    event_handler = NULL;
}
