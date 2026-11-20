/* The unit record (unit_logic.h), read from flash once at start and kept. The firmware never writes it. */
#pragma once

#include "unit_logic.h"

/* Reads the sector at UNIT_ADDR (into a heap buffer, freed after the parse), checks it against this
 * chip's MAC and keeps the result. Call once in app_main, before the web server starts; later calls do
 * nothing. */
void unitrec_load(void);

/* What unitrec_load found: UNIT_NONE before it ran, or when the sector could not be read. *out is
 * filled on UNIT_OK (zeroed otherwise). Any task. */
unit_status_t unitrec_get(unit_t *out);
