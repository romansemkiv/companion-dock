#pragma once
#include "deckmodel/model.h"

/* Companion layout manifest (unformatted JSON). Caller free()s the result; NULL on failure. */
char *deck_manifest_json(const deck_model_t *m);
