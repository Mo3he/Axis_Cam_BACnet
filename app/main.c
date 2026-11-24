/*
 * BACnet Server ACAP entry binary.
 *
 * Device settings are manifest parameters; the object list is a JSON file in
 * localdata. Both are served on 127.0.0.1:2213 behind the camera's
 * authenticated reverse proxy (/local/BACnet_Server/api/...). The BACnet/IP
 * device and the event subscriptions run in the GLib main loop.
 * The loopback port must be unique per device; see PARAM_CGI_FALLBACK.md.
 */

#include <axsdk/axparameter.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "bacnet_device.h"
#include "bindings.h"
#include "cJSON.h"
#include "objects_config.h"

#define APP_NAME "BACnet_Server"
#define APP_DIR "/usr/local/packages/" APP_NAME
#define OBJECTS_FILE APP_DIR "/localdata/objects.json"
#define REVISION_FILE APP_DIR "/localdata/database_revision"
#define SETTINGS_HTTP_PORT 2213
#define MAX_REQUEST (256 * 1024)
#define RESTART_DELAY_MS 500
#define DEFAULT_UDP_PORT 47808

typedef struct {
    const char *name;
    const char *fallback;
    char *value;
} Parameter;

/* Must stay in sync with paramConfig in manifest.json. */
static Parameter parameters[] = {
    {"Enabled", "no", NULL},   {"DeviceInstance", "", NULL}, {"DeviceName", "", NULL},
    {"UdpPort", "47808", NULL}, {"Location", "", NULL},       {"Description", "", NULL},
};

static AXParameter *parameter_handle;
static GMainLoop *main_loop;
static guint restart_timer;
static ObjectsConfig active_objects;
static DeviceSettings identity; /* model, serial, firmware, version */
static DeviceSettings effective;
static char start_error[160];
static uint32_t database_revision = 1;

/* ------------------------------------------------------------- parameters */

static Parameter *find_parameter(const char *name) {
    const char *last = strrchr(name, '.');
    if (last)
        name = last + 1;
    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++) {
        if (g_strcmp0(parameters[i].name, name) == 0)
            return &parameters[i];
    }
    return NULL;
}

static const char *parameter_value(const char *name) {
    Parameter *parameter = find_parameter(name);
    if (!parameter)
        return "";
    return parameter->value ? parameter->value : parameter->fallback;
}

static void load_parameter(Parameter *parameter) {
    gchar *value = NULL;
    if (ax_parameter_get(parameter_handle, parameter->name, &value, NULL)) {
        g_free(parameter->value);
        parameter->value = g_strdup(value ? value : "");
    } else if (!parameter->value) {
        parameter->value = g_strdup(parameter->fallback);
    }
    g_free(value);
}

static void read_device_parameter(const char *name, char *out, size_t out_size) {
    gchar *value = NULL;
    out[0] = '\0';
    if (ax_parameter_get(parameter_handle, name, &value, NULL) && value)
        snprintf(out, out_size, "%s", value);
    g_free(value);
}

static void load_identity(void) {
    read_device_parameter("root.Brand.ProdShortName", identity.model, sizeof(identity.model));
    read_device_parameter("root.Properties.System.SerialNumber", identity.serial, sizeof(identity.serial));
    read_device_parameter("root.Properties.Firmware.Version", identity.firmware, sizeof(identity.firmware));

    cJSON *manifest = NULL;
    gchar *text = NULL;
    if (g_file_get_contents(APP_DIR "/manifest.json", &text, NULL, NULL))
        manifest = cJSON_Parse(text);
    const cJSON *setup = cJSON_GetObjectItem(cJSON_GetObjectItem(manifest, "acapPackageConf"), "setup");
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(setup, "version"));
    snprintf(identity.app_version, sizeof(identity.app_version), "%s", version ? version : "");
    cJSON_Delete(manifest);
    g_free(text);
}

/* Default instance from the last hex digits of the serial number (the MAC),
 * so cameras on one network get different instances without setup. */
static uint32_t auto_instance(void) {
    size_t length = strlen(identity.serial);
    const char *tail = length > 6 ? identity.serial + length - 6 : identity.serial;
    uint32_t value = (uint32_t)strtoul(tail, NULL, 16) & 0x3FFFFF;
    return value > MAX_OBJECT_INSTANCE ? MAX_OBJECT_INSTANCE : value;
}

static bool parse_uint(const char *text, unsigned long min, unsigned long max, unsigned long *out) {
    char *end = NULL;
    if (!text || !*text)
        return false;
    unsigned long value = strtoul(text, &end, 10);
    if (*end || value < min || value > max)
        return false;
    *out = value;
    return true;
}

static void resolve_settings(DeviceSettings *out) {
    unsigned long number;
    *out = identity;

    out->instance =
        parse_uint(parameter_value("DeviceInstance"), 0, MAX_OBJECT_INSTANCE, &number) ? (uint32_t)number
                                                                                        : auto_instance();
    out->port = parse_uint(parameter_value("UdpPort"), 1024, 65535, &number) ? (uint16_t)number
                                                                               : DEFAULT_UDP_PORT;
    if (*parameter_value("DeviceName"))
        snprintf(out->name, sizeof(out->name), "%s", parameter_value("DeviceName"));
    else
        snprintf(out->name, sizeof(out->name), "%.32s %.31s", identity.model[0] ? identity.model : "Axis",
                 identity.serial);
    snprintf(out->location, sizeof(out->location), "%s", parameter_value("Location"));
    snprintf(out->description, sizeof(out->description), "%s", parameter_value("Description"));
}

/* ---------------------------------------------------------------- objects */

static bool load_objects(ObjectsConfig *out) {
    gchar *text = NULL;
    char error[160];
    out->count = 0;
    if (!g_file_get_contents(OBJECTS_FILE, &text, NULL, NULL))
        return true; /* no objects yet */
    cJSON *json = cJSON_Parse(text);
    g_free(text);
    bool ok = objects_config_parse(json, out, error, sizeof(error));
    cJSON_Delete(json);
    if (!ok) {
        syslog(LOG_ERR, "%s is invalid and was ignored: %s", OBJECTS_FILE, error);
        out->count = 0;
    }
    return ok;
}

static bool save_objects(const ObjectsConfig *config) {
    cJSON *json = objects_config_to_json(config);
    char *text = cJSON_Print(json);
    GError *error = NULL;
    bool ok = text && g_file_set_contents(OBJECTS_FILE, text, -1, &error);
    if (!ok)
        syslog(LOG_ERR, "saving %s failed: %s", OBJECTS_FILE, error ? error->message : "out of memory");
    g_clear_error(&error);
    cJSON_free(text);
    cJSON_Delete(json);
    return ok;
}

/* BACnet clients re-read the object list when Database_Revision changes, so
 * keep it across restarts and bump it on every configuration change. */
static void load_revision(void) {
    gchar *text = NULL;
    if (g_file_get_contents(REVISION_FILE, &text, NULL, NULL))
        database_revision = (uint32_t)strtoul(text, NULL, 10);
    g_free(text);
}

static void bump_revision(void) {
    gchar *text = g_strdup_printf("%u\n", ++database_revision);
    g_file_set_contents(REVISION_FILE, text, -1, NULL);
    g_free(text);
}

/* ----------------------------------------------------------------- device */

static void stop_device(void) {
    bindings_stop();
    bacnet_device_stop();
}

static void start_device(void) {
    stop_device();
    start_error[0] = '\0';
    resolve_settings(&effective);
    effective.database_revision = database_revision;
    if (g_strcmp0(parameter_value("Enabled"), "yes") != 0) {
        syslog(LOG_INFO, "BACnet device disabled");
        return;
    }
    if (!bacnet_device_start(&effective, &active_objects, start_error, sizeof(start_error))) {
        syslog(LOG_ERR, "BACnet device not started: %s", start_error);
        return;
    }
    bindings_start(&active_objects);
}

static gboolean on_restart_timeout(gpointer user_data) {
    (void)user_data;
    restart_timer = 0;
    start_device();
    return G_SOURCE_REMOVE;
}

/* Saving several parameters fires one callback each; restart once. */
static void schedule_restart(void) {
    if (!restart_timer)
        bump_revision();
    if (restart_timer)
        g_source_remove(restart_timer);
    restart_timer = g_timeout_add(RESTART_DELAY_MS, on_restart_timeout, NULL);
}

static void on_parameter_changed(const gchar *name, const gchar *value, gpointer user_data) {
    (void)user_data;
    Parameter *parameter = find_parameter(name);
    if (!parameter || g_strcmp0(parameter->value, value) == 0)
        return;
    g_free(parameter->value);
    parameter->value = g_strdup(value ? value : "");
    schedule_restart();
}

/* -------------------------------------------------------------------- API */

static cJSON *settings_json(void) {
    DeviceSettings resolved;
    resolve_settings(&resolved);
    cJSON *json = cJSON_CreateObject();
    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++)
        cJSON_AddStringToObject(json, parameters[i].name, parameter_value(parameters[i].name));
    cJSON *defaults = cJSON_AddObjectToObject(json, "effective");
    cJSON_AddNumberToObject(defaults, "DeviceInstance", resolved.instance);
    cJSON_AddStringToObject(defaults, "DeviceName", resolved.name);
    cJSON_AddNumberToObject(defaults, "UdpPort", resolved.port);
    return json;
}

static cJSON *status_json(void) {
    char address[32];
    cJSON *json = cJSON_CreateObject();
    bacnet_device_address(address, sizeof(address));
    cJSON_AddBoolToObject(json, "enabled", g_strcmp0(parameter_value("Enabled"), "yes") == 0);
    cJSON_AddBoolToObject(json, "running", bacnet_device_running());
    cJSON_AddStringToObject(json, "error", start_error);
    cJSON *device = cJSON_AddObjectToObject(json, "device");
    cJSON_AddNumberToObject(device, "instance", effective.instance);
    cJSON_AddStringToObject(device, "name", effective.name);
    cJSON_AddStringToObject(device, "address", address);
    cJSON_AddNumberToObject(device, "port", effective.port);
    cJSON_AddStringToObject(device, "model", identity.model);
    cJSON_AddStringToObject(device, "version", identity.app_version);
    cJSON_AddItemToObject(json, "objects", bindings_status());
    return json;
}

static gchar *url_decode(const char *value, gsize length) {
    GString *out = g_string_sized_new(length);
    for (gsize i = 0; i < length; i++) {
        if (value[i] == '+') {
            g_string_append_c(out, ' ');
        } else if (value[i] == '%' && i + 2 < length && g_ascii_isxdigit(value[i + 1]) &&
                   g_ascii_isxdigit(value[i + 2])) {
            g_string_append_c(out, (gchar)(g_ascii_xdigit_value(value[i + 1]) * 16 +
                                           g_ascii_xdigit_value(value[i + 2])));
            i += 2;
        } else {
            g_string_append_c(out, value[i]);
        }
    }
    /* g_string_free(out, FALSE) can inline to g_string_free_and_steal(),
     * missing on older AXIS OS; copy the buffer instead. */
    gchar *decoded = g_strdup(out->str);
    g_string_free(out, TRUE);
    return decoded;
}

static bool validate_setting(const char *name, const char *value, char *error, size_t error_size) {
    unsigned long number;
    if (g_strcmp0(name, "Enabled") == 0 && g_strcmp0(value, "yes") != 0 && g_strcmp0(value, "no") != 0)
        snprintf(error, error_size, "Enabled must be yes or no");
    else if (g_strcmp0(name, "DeviceInstance") == 0 && *value &&
             !parse_uint(value, 0, MAX_OBJECT_INSTANCE, &number))
        snprintf(error, error_size, "DeviceInstance must be empty (automatic) or 0 to 4194302");
    else if (g_strcmp0(name, "UdpPort") == 0 && !parse_uint(value, 1024, 65535, &number))
        snprintf(error, error_size, "UdpPort must be 1024 to 65535");
    else if (g_strcmp0(name, "DeviceName") == 0 && strlen(value) > DEVICE_NAME_MAX_LEN)
        snprintf(error, error_size, "DeviceName is longer than 64 characters");
    else if ((g_strcmp0(name, "Location") == 0 || g_strcmp0(name, "Description") == 0) &&
             strlen(value) > DEVICE_TEXT_MAX_LEN)
        snprintf(error, error_size, "%s is longer than 64 characters", name);
    else
        return true;
    return false;
}

/* application/x-www-form-urlencoded; validates everything before writing. */
static bool apply_settings_body(const char *body, char *error, size_t error_size) {
    gchar **pairs = g_strsplit(body ? body : "", "&", -1);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    bool ok = true;

    for (gchar **pair = pairs; *pair && ok; pair++) {
        gchar *separator = strchr(*pair, '=');
        if (!separator)
            continue;
        gchar *name = url_decode(*pair, (gsize)(separator - *pair));
        gchar *value = url_decode(separator + 1, strlen(separator + 1));
        if (!find_parameter(name)) {
            snprintf(error, error_size, "unknown setting %s", name);
            ok = false;
        } else {
            ok = validate_setting(name, value, error, error_size);
        }
        g_ptr_array_add(names, name);
        g_ptr_array_add(values, value);
    }

    for (guint i = 0; ok && i < names->len; i++) {
        Parameter *parameter = find_parameter(names->pdata[i]);
        const char *value = values->pdata[i];
        GError *set_error = NULL;
        if (g_strcmp0(parameter->value, value) == 0)
            continue;
        if (ax_parameter_set(parameter_handle, parameter->name, value, TRUE, &set_error)) {
            g_free(parameter->value);
            parameter->value = g_strdup(value);
            schedule_restart();
        } else {
            snprintf(error, error_size, "could not save %s: %s", parameter->name,
                     set_error ? set_error->message : "unknown error");
            ok = false;
        }
        g_clear_error(&set_error);
    }

    g_ptr_array_free(names, TRUE);
    g_ptr_array_free(values, TRUE);
    g_strfreev(pairs);
    return ok;
}

static bool apply_objects_body(const char *body, char *error, size_t error_size) {
    cJSON *json = cJSON_Parse(body ? body : "");
    if (!json) {
        snprintf(error, error_size, "body is not valid JSON");
        return false;
    }
    ObjectsConfig *parsed = g_new0(ObjectsConfig, 1);
    bool ok = objects_config_parse(json, parsed, error, error_size);
    cJSON_Delete(json);
    if (ok && !save_objects(parsed)) {
        snprintf(error, error_size, "could not save the objects");
        ok = false;
    }
    if (ok) {
        /* Objects point into active_objects, so stop before replacing it. */
        stop_device();
        active_objects = *parsed;
        bump_revision();
        start_device();
    }
    g_free(parsed);
    return ok;
}

static void send_response(GOutputStream *out, const char *status, const char *content_type, const char *body) {
    gchar *header = g_strdup_printf("HTTP/1.1 %s\r\n"
                                    "Content-Type: %s\r\n"
                                    "Content-Length: %zu\r\n"
                                    "Cache-Control: no-store\r\n"
                                    "Connection: close\r\n"
                                    "\r\n",
                                    status, content_type, strlen(body));
    g_output_stream_write_all(out, header, strlen(header), NULL, NULL, NULL);
    g_output_stream_write_all(out, body, strlen(body), NULL, NULL, NULL);
    g_free(header);
}

static void send_json(GOutputStream *out, const char *status, cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    send_response(out, status, "application/json", text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(json);
}

static void send_error(GOutputStream *out, const char *status, const char *message) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    send_json(out, status, json);
}

/* "GET /local/BACnet_Server/api/status?x HTTP/1.1" -> "status". The reverse
 * proxy forwards the full path, so take what follows the last "api/". */
static gchar *request_route(const char *request_line) {
    const char *path = strchr(request_line, ' ');
    if (!path)
        return g_strdup("");
    path++;
    size_t length = strcspn(path, " ?");
    gchar *route = g_strndup(path, length);
    const char *start = g_strrstr(route, "/api/");
    if (start)
        start += strlen("/api/");
    else
        for (start = route; *start == '/'; start++)
            ;
    gchar *result = g_strdup(start);
    g_free(route);
    return result;
}

static void handle_request(GOutputStream *out, const char *request_line, const char *body) {
    gboolean is_get = g_str_has_prefix(request_line, "GET ");
    gboolean is_post = g_str_has_prefix(request_line, "POST ");
    gchar *route = request_route(request_line);
    char error[256] = "";

    if (is_get && g_strcmp0(route, "settings") == 0) {
        send_json(out, "200 OK", settings_json());
    } else if (is_post && g_strcmp0(route, "settings") == 0) {
        if (apply_settings_body(body, error, sizeof(error)))
            send_response(out, "200 OK", "application/json", "{\"status\":\"ok\"}");
        else
            send_error(out, "400 Bad Request", error);
    } else if (is_get && g_strcmp0(route, "objects") == 0) {
        send_json(out, "200 OK", objects_config_to_json(&active_objects));
    } else if (is_post && g_strcmp0(route, "objects") == 0) {
        if (apply_objects_body(body, error, sizeof(error)))
            send_response(out, "200 OK", "application/json", "{\"status\":\"ok\"}");
        else
            send_error(out, "400 Bad Request", error);
    } else if (is_get && g_strcmp0(route, "status") == 0) {
        send_json(out, "200 OK", status_json());
    } else {
        syslog(LOG_WARNING, "API: no route for '%s'", request_line);
        send_error(out, "404 Not Found", "not found");
    }
    g_free(route);
}

static gboolean on_request(GSocketService *service, GSocketConnection *connection, GObject *source,
                           gpointer user_data) {
    (void)service;
    (void)source;
    (void)user_data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gchar *buffer = g_malloc(MAX_REQUEST);
    gsize total = 0;
    gsize header_length = 0;
    glong content_length = -1;

    while (total < MAX_REQUEST - 1) {
        gssize got = g_input_stream_read(in, buffer + total, MAX_REQUEST - 1 - total, NULL, NULL);
        if (got <= 0)
            break;
        total += (gsize)got;
        buffer[total] = '\0';
        gchar *header_end = strstr(buffer, "\r\n\r\n");
        if (!header_end)
            continue;
        header_length = (gsize)(header_end - buffer) + 4;
        if (content_length < 0) {
            gchar *headers = g_ascii_strdown(buffer, (gssize)header_length);
            gchar *field = strstr(headers, "content-length:");
            content_length = field ? strtol(field + strlen("content-length:"), NULL, 10) : 0;
            g_free(headers);
        }
        if (total - header_length >= (gsize)content_length)
            break;
    }
    buffer[total] = '\0';

    if (header_length == 0) {
        send_error(out, "400 Bad Request", "bad request");
    } else if (content_length > 0 && total - header_length < (gsize)content_length) {
        send_error(out, "413 Payload Too Large", "request too large");
    } else {
        gchar *line_end = strstr(buffer, "\r\n");
        gchar *request_line = g_strndup(buffer, (gsize)(line_end - buffer));
        handle_request(out, request_line, buffer + header_length);
        g_free(request_line);
    }

    g_free(buffer);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
}

static gboolean start_api_server(void) {
    GError *error = NULL;
    GSocketService *service = g_socket_service_new();
    GInetAddress *address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *socket_address = g_inet_socket_address_new(address, SETTINGS_HTTP_PORT);
    gboolean added = g_socket_listener_add_address(G_SOCKET_LISTENER(service), socket_address,
                                                   G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, NULL, &error);
    g_object_unref(socket_address);
    g_object_unref(address);

    if (!added) {
        /* Usually another ACAP claimed the port, which would silently send
         * this app's API to that app. */
        syslog(LOG_ERR, "API server cannot bind 127.0.0.1:%d: %s", SETTINGS_HTTP_PORT,
               error ? error->message : "unknown error");
        g_clear_error(&error);
        g_object_unref(service);
        return FALSE;
    }
    g_signal_connect(service, "incoming", G_CALLBACK(on_request), NULL);
    g_socket_service_start(service);
    return TRUE;
}

/* ------------------------------------------------------------------- main */

static gboolean on_terminate(gpointer user_data) {
    (void)user_data;
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    GError *error = NULL;

    openlog(APP_NAME, LOG_PID, LOG_USER);
    parameter_handle = ax_parameter_new(APP_NAME, &error);
    if (!parameter_handle) {
        syslog(LOG_ERR, "ax_parameter_new failed: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return EXIT_FAILURE;
    }
    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++) {
        load_parameter(&parameters[i]);
        if (!ax_parameter_register_callback(parameter_handle, parameters[i].name, on_parameter_changed, NULL,
                                            &error))
            syslog(LOG_WARNING, "callback for %s failed: %s", parameters[i].name,
                   error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    load_identity();
    load_revision();
    load_objects(&active_objects);

    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_terminate, NULL);
    g_unix_signal_add(SIGINT, on_terminate, NULL);

    bindings_init();
    start_api_server();
    start_device();
    syslog(LOG_INFO, "%s %s started", APP_NAME, identity.app_version);
    g_main_loop_run(main_loop);

    stop_device();
    bindings_cleanup();
    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++)
        g_free(parameters[i].value);
    ax_parameter_free(parameter_handle);
    g_main_loop_unref(main_loop);
    closelog();
    return EXIT_SUCCESS;
}
