#include "bindings.h"

#include <axsdk/axevent.h>
#include <curl/curl.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "bacnet_device.h"

#define SEED_ATTEMPTS 12
#define SEED_RETRY_SECONDS 5

typedef struct {
    const ObjectConfig *object;
    guint subscription;
    bool subscribed;
    bool has_value; /* a converted value arrived since the object was created */
    char error[192];
    char last_raw[VALUE_MAX_LEN + 1];
    gint64 last_update; /* Unix time, 0 = no event yet */
    unsigned long events;
} BindingState;

static AXEventHandler *event_handler;
static BindingState states[MAX_OBJECTS];
static int state_count;
/* Bumped on every start and stop so late seed results are dropped. */
static unsigned generation;
static GThreadPool *seed_pool;
static void seed_worker(gpointer data, gpointer user_data);

bool bindings_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    seed_pool = g_thread_pool_new(seed_worker, NULL, 2, FALSE, NULL);
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
        if (!state->has_value)
            bacnet_device_set_health(object, OBJECT_OK);
        state->has_value = true;
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

/* Initial values for AOA occupancy objects (see binding_aoa_scenario). The
 * HTTP request runs on a worker thread so BACnet traffic is never blocked;
 * the result is applied on the main loop, unless an event came first. */
typedef struct {
    unsigned generation;
    int index;
    int scenario;
    int attempt;
    char *response;
} SeedJob;

static size_t collect(char *data, size_t size, size_t count, void *user_data) {
    g_string_append_len(user_data, data, (gssize)(size * count));
    return size * count;
}

static char *vapix_credentials(void) {
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    GVariant *result = NULL;
    char *credentials = NULL;

    if (bus)
        result = g_dbus_connection_call_sync(bus, "com.axis.HTTPConf1", "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
                                             "com.axis.HTTPConf1.VAPIXServiceAccounts1", "GetCredentials",
                                             g_variant_new("(s)", "bacnet-server"), G_VARIANT_TYPE("(s)"),
                                             G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (result) {
        g_variant_get(result, "(s)", &credentials);
        g_variant_unref(result);
    } else {
        syslog(LOG_WARNING, "could not get VAPIX credentials: %s", error ? error->message : "unknown error");
    }
    g_clear_error(&error);
    g_clear_object(&bus);
    return credentials;
}

static char *fetch_occupancy(int scenario) {
    char *credentials = vapix_credentials();
    CURL *curl;
    GString *body;
    struct curl_slist *headers = NULL;
    char request[128];
    long status = 0;
    CURLcode result;

    if (!credentials)
        return NULL;
    curl = curl_easy_init();
    if (!curl) {
        g_free(credentials);
        return NULL;
    }
    body = g_string_new(NULL);
    snprintf(request, sizeof(request),
             "{\"method\":\"getOccupancy\",\"apiVersion\":\"1.0\",\"params\":{\"scenario\":%d}}", scenario);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.12/local/objectanalytics/control.cgi");
    curl_easy_setopt(curl, CURLOPT_USERPWD, credentials);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    g_free(credentials);

    if (result != CURLE_OK || status != 200) {
        g_string_free(body, TRUE);
        return NULL;
    }
    return g_string_free(body, FALSE);
}

static void free_seed(SeedJob *job) {
    g_free(job->response);
    g_free(job);
}

static bool seed_wanted(const SeedJob *job) {
    return job->generation == generation && job->index < state_count && !states[job->index].has_value;
}

static gboolean seed_done(gpointer user_data);

static gboolean seed_retry(gpointer user_data) {
    SeedJob *job = user_data;
    if (seed_wanted(job))
        g_thread_pool_push(seed_pool, job, NULL);
    else
        free_seed(job);
    return G_SOURCE_REMOVE;
}

static void seed_worker(gpointer data, gpointer user_data) {
    (void)user_data;
    SeedJob *job = data;
    g_free(job->response);
    job->response = fetch_occupancy(job->scenario);
    g_idle_add(seed_done, job);
}

static gboolean seed_done(gpointer user_data) {
    SeedJob *job = user_data;
    cJSON *response;
    EventValue value;

    if (!seed_wanted(job)) {
        free_seed(job);
        return G_SOURCE_REMOVE;
    }
    BindingState *state = &states[job->index];
    response = job->response ? cJSON_Parse(job->response) : NULL;
    if (aoa_occupancy_value(response, state->object->binding.data_key, &value)) {
        state->last_update = g_get_real_time() / G_USEC_PER_SEC;
        apply_value(state, &value);
        free_seed(job);
    } else if (cJSON_GetObjectItemCaseSensitive(response, "data") ||
               cJSON_GetObjectItemCaseSensitive(response, "error")) {
        /* Object Analytics answered, but not with this count (e.g. not an
         * occupancy scenario): wait for events as usual. */
        free_seed(job);
    } else if (++job->attempt < SEED_ATTEMPTS) {
        /* Object Analytics not reachable yet, e.g. still starting. */
        g_timeout_add_seconds(SEED_RETRY_SECONDS, seed_retry, job);
    } else {
        syslog(LOG_INFO, "%s %u: could not read the current count from Object Analytics",
               object_kind_name(state->object->kind), state->object->instance);
        free_seed(job);
    }
    cJSON_Delete(response);
    return G_SOURCE_REMOVE;
}

static void seed_start(int index) {
    int scenario;
    if (!seed_pool || !binding_aoa_scenario(&states[index].object->binding, &scenario))
        return;
    SeedJob *job = g_new0(SeedJob, 1);
    job->generation = generation;
    job->index = index;
    job->scenario = scenario;
    g_thread_pool_push(seed_pool, job, NULL);
}

void bindings_start(const ObjectsConfig *objects) {
    bindings_stop();
    for (int i = 0; i < objects->count; i++) {
        BindingState *state = &states[i];
        memset(state, 0, sizeof(*state));
        state->object = &objects->objects[i];
        /* Before subscribing: stateful events deliver their current state
         * from inside the subscribe call. */
        bacnet_device_set_health(state->object, OBJECT_NO_DATA);
        state->subscribed = event_handler && subscribe(state);
        if (!state->subscribed)
            bacnet_device_set_health(state->object, OBJECT_CONFIG_ERROR);
    }
    state_count = objects->count;
    for (int i = 0; i < state_count; i++)
        if (states[i].subscribed && !states[i].has_value)
            seed_start(i);
}

void bindings_stop(void) {
    generation++;
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
        cJSON_AddBoolToObject(item, "has_value", state->has_value);
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
