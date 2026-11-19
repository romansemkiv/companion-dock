/* The web UI's extension points: what another component in the firmware can add to the API without webui knowing
 * it. Registration happens at start-up, before the web server serves requests (webui_start); it is not guarded
 * against calls running at the same time, nor against an API call in progress. What a call passes must stay valid
 * for the program's life (static data, string literals), as rpc_register's names. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Settings another component owns, inside a section's *.GetConfig / *.SetConfig ---- */

#define WEBUI_CFG_EXT_MAX 8

/* Keys of one section that another component stores and applies itself. *.GetConfig adds get's keys after the
 * section's own. *.SetConfig hands the keys to their owners and the rest to the section, all or nothing: first every
 * owner's set with dry_run true (a refusal: the call fails with its err, nothing is saved), then the section's own
 * keys, when any came, are saved (or checked, with the caller's dry_run), then, unless the caller's dry_run, every
 * owner's set with dry_run false (a failure there: "saving the settings failed: <err>"). get and set run on the web
 * server's task, without webui's config lock. */
typedef struct {
    const char *section;          /* "Device", "Companion", "Eth", "Wifi", "Ap", "Feedback" */
    const char *const *keys;      /* the keys it owns, NULL-terminated; never a built-in key of that section */
    void (*get)(cJSON *out);      /* adds its keys' values to the section's GetConfig result */
    /* params holds only its keys. Validates all of them; applies (saves) only when !dry_run. All or nothing. */
    bool (*set)(const cJSON *params, bool dry_run, char *err, size_t errlen);
} webui_cfg_ext_t;

/* Adds x (kept by pointer). At most WEBUI_CFG_EXT_MAX; false when full or x is incomplete. */
bool webui_cfg_ext_register(const webui_cfg_ext_t *x);

/* ---- Device.GetInfo ---- */

/* Device.GetInfo "edition": "standard" unless set; NULL sets "standard" again. name is kept by pointer. */
void webui_set_edition(const char *name);

#ifdef __cplusplus
}
#endif
