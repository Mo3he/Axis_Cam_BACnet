#!/bin/sh
# Host unit tests: object declarations and event value conversion.
set -eu
cd "$(dirname "$0")/.."
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iapp \
	-o /tmp/objects_config_test tests/objects_config_test.c app/objects_config.c app/cJSON.c -lm
/tmp/objects_config_test
