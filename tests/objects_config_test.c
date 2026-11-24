/* Host tests for app/objects_config.c: parsing and validation of object
 * declarations, JSON round trip, and conversion of event values. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "objects_config.h"

static int failures = 0;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

static const char *VALID =
    "{\"objects\":["
    "{\"type\":\"binary-value\",\"instance\":1,\"name\":\"Door\",\"active_text\":\"Open\",\"invert\":true,"
    "\"event\":{\"topic\":[\"tns1:Device\",\"tnsaxis:IO\",\"tnsaxis:VirtualInput\"],\"source_key\":\"port\","
    "\"source_value\":\"1\",\"data_key\":\"active\"}},"
    "{\"type\":\"analog-value\",\"instance\":1,\"name\":\"Temp\",\"units\":62,\"cov_increment\":0.5,"
    "\"event\":{\"topic\":[\"tnsaxis:Storage\",\"tnsaxis:Alert\"],\"data_key\":\"temperature\"}},"
    "{\"type\":\"multistate-value\",\"instance\":7,\"name\":\"Recorder\",\"states\":[\"Recording\",\"Stopped\"],"
    "\"event\":{\"topic\":[\"tnsaxis:Storage\",\"tnsaxis:RecorderStatus\"],\"data_key\":\"status\"}}]}";

static bool parse_text(const char *text, ObjectsConfig *config, char *error, size_t size) {
    cJSON *json = cJSON_Parse(text);
    bool ok = objects_config_parse(json, config, error, size);
    cJSON_Delete(json);
    return ok;
}

static void test_valid_and_round_trip(void) {
    static ObjectsConfig config, again;
    char error[256] = "";
    CHECK(parse_text(VALID, &config, error, sizeof(error)), "valid config rejected: %s", error);
    CHECK(config.count == 3, "count %d", config.count);
    CHECK(config.objects[0].invert && strcmp(config.objects[0].active_text, "Open") == 0, "binary fields");
    CHECK(config.objects[0].binding.topic_count == 3, "topic levels");
    CHECK(strcmp(config.objects[0].binding.topic[2].ns, "tnsaxis") == 0, "topic namespace");
    CHECK(config.objects[1].units == 62 && fabs(config.objects[1].cov_increment - 0.5) < 1e-9, "analog fields");
    CHECK(config.objects[2].state_count == 2, "states");

    char path[128];
    binding_topic_path(&config.objects[0].binding, path, sizeof(path));
    CHECK(strcmp(path, "tns1:Device/tnsaxis:IO/tnsaxis:VirtualInput") == 0, "path %s", path);

    cJSON *json = objects_config_to_json(&config);
    CHECK(objects_config_parse(json, &again, error, sizeof(error)), "round trip rejected: %s", error);
    CHECK(memcmp(&config, &again, sizeof(config)) == 0, "round trip changed the config");
    cJSON_Delete(json);
}

static void expect_error(const char *text, const char *expected) {
    static ObjectsConfig config;
    char error[256] = "";
    bool ok = parse_text(text, &config, error, sizeof(error));
    CHECK(!ok, "accepted: %s", text);
    CHECK(strstr(error, expected) != NULL, "error '%s' does not contain '%s'", error, expected);
}

#define EVENT "\"event\":{\"topic\":[\"tns1:Device\"],\"data_key\":\"active\"}"

static void test_rejections(void) {
    expect_error("{}", "objects: must be an array");
    expect_error("{\"objects\":[{\"type\":\"binary-input\",\"instance\":1,\"name\":\"a\"," EVENT "}]}",
                 "objects[0].type");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":4194303,\"name\":\"a\"," EVENT "}]}",
                 "objects[0].instance");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1.5,\"name\":\"a\"," EVENT "}]}",
                 "objects[0].instance");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"\"," EVENT "}]}",
                 "objects[0].name: must not be empty");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\"}]}",
                 "objects[0].event: is required");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\","
                 "\"event\":{\"topic\":[\"Device\"],\"data_key\":\"active\"}}]}",
                 "objects[0].event.topic[0]");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\","
                 "\"event\":{\"topic\":[\"onvif:Device\"],\"data_key\":\"active\"}}]}",
                 "namespace must be tns1 or tnsaxis");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\","
                 "\"event\":{\"topic\":[\"tns1:Device\"]}}]}",
                 "objects[0].event.data_key: is required");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\","
                 "\"event\":{\"topic\":[\"tns1:Device\"],\"data_key\":\"active\",\"source_value\":\"1\"}}]}",
                 "event.source_key: is required when source_value is set");
    expect_error("{\"objects\":[{\"type\":\"multistate-value\",\"instance\":1,\"name\":\"a\",\"states\":[\"x\"],"
                 EVENT "}]}",
                 "objects[0].states");
    expect_error("{\"objects\":[{\"type\":\"multistate-value\",\"instance\":1,\"name\":\"a\","
                 "\"states\":[\"On\",\"on\"]," EVENT "}]}",
                 "objects[0].states[1]: duplicates");
    expect_error("{\"objects\":[{\"type\":\"analog-value\",\"instance\":1,\"name\":\"a\",\"cov_increment\":-1,"
                 EVENT "}]}",
                 "objects[0].cov_increment");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\"," EVENT "},"
                 "{\"type\":\"binary-value\",\"instance\":1,\"name\":\"b\"," EVENT "}]}",
                 "objects[1].instance: is already used");
    expect_error("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\"," EVENT "},"
                 "{\"type\":\"analog-value\",\"instance\":1,\"name\":\"a\"," EVENT "}]}",
                 "objects[1].name: is already used");

    /* Same instance on different object types is allowed. */
    static ObjectsConfig config;
    char error[256] = "";
    CHECK(parse_text("{\"objects\":[{\"type\":\"binary-value\",\"instance\":1,\"name\":\"a\"," EVENT "},"
                     "{\"type\":\"analog-value\",\"instance\":1,\"name\":\"b\"," EVENT "}]}",
                     &config, error, sizeof(error)),
          "same instance on different types rejected: %s", error);
}

static EventValue string_value(const char *text) {
    EventValue value = {.type = EV_STRING};
    snprintf(value.s, sizeof(value.s), "%s", text);
    return value;
}

static void test_conversions(void) {
    double number;
    bool active;
    uint32_t state;
    EventValue b = {.type = EV_BOOL, .b = true};
    EventValue i = {.type = EV_INT, .i = 42};
    EventValue d = {.type = EV_DOUBLE, .d = 21.5};

    CHECK(event_value_to_analog(&b, &number) && number == 1.0, "bool to analog");
    CHECK(event_value_to_analog(&i, &number) && number == 42.0, "int to analog");
    CHECK(event_value_to_analog(&d, &number) && number == 21.5, "double to analog");
    EventValue s = string_value(" 3.25 ");
    CHECK(event_value_to_analog(&s, &number) && number == 3.25, "string to analog");
    s = string_value("warm");
    CHECK(!event_value_to_analog(&s, &number), "non-numeric string accepted");

    CHECK(event_value_to_binary(&b, false, &active) && active, "bool true");
    CHECK(event_value_to_binary(&b, true, &active) && !active, "inverted");
    EventValue zero = {.type = EV_INT, .i = 0};
    CHECK(event_value_to_binary(&zero, false, &active) && !active, "int zero");
    s = string_value("Active");
    CHECK(event_value_to_binary(&s, false, &active) && active, "string active");
    s = string_value("off");
    CHECK(event_value_to_binary(&s, false, &active) && !active, "string off");
    s = string_value("maybe");
    CHECK(!event_value_to_binary(&s, false, &active), "unknown string accepted");

    ObjectConfig msv = {.kind = OBJ_MULTISTATE_VALUE, .state_count = 3};
    snprintf(msv.states[0], sizeof(msv.states[0]), "Recording");
    snprintf(msv.states[1], sizeof(msv.states[1]), "Stopped");
    snprintf(msv.states[2], sizeof(msv.states[2]), "Idle");
    s = string_value("stopped");
    CHECK(event_value_to_state(&msv, &s, &state) && state == 2, "state by text");
    EventValue three = {.type = EV_INT, .i = 3};
    CHECK(event_value_to_state(&msv, &three, &state) && state == 3, "state by number");
    EventValue four = {.type = EV_INT, .i = 4};
    CHECK(!event_value_to_state(&msv, &four, &state), "state out of range accepted");
    CHECK(!event_value_to_state(&msv, &zero, &state), "state zero accepted");
    s = string_value("Paused");
    CHECK(!event_value_to_state(&msv, &s, &state), "unknown state accepted");

    char text[32];
    event_value_to_string(&b, text, sizeof(text));
    CHECK(strcmp(text, "1") == 0, "bool as source filter text: %s", text);
    event_value_to_string(&i, text, sizeof(text));
    CHECK(strcmp(text, "42") == 0, "int as source filter text: %s", text);
}

int main(void) {
    test_valid_and_round_trip();
    test_rejections();
    test_conversions();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("objects_config: all tests passed\n");
    return 0;
}
