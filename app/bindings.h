/*
 * Event bindings: one axevent subscription per BACnet object. Each event
 * that matches the object's optional source filter sets its present value.
 */
#ifndef BINDINGS_H
#define BINDINGS_H

#include <stdbool.h>

#include "cJSON.h"
#include "objects_config.h"

bool bindings_init(void);
/* Subscribes every object. The objects config must stay valid until
 * bindings_stop(). */
void bindings_start(const ObjectsConfig *objects);
void bindings_stop(void);
/* Per-object status for the settings page. */
cJSON *bindings_status(void);
void bindings_cleanup(void);

#endif
