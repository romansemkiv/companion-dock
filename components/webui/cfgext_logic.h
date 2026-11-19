/* A *.SetConfig's params split between the section's built-in keys and the keys other components own there
 * (webui/ext.h), pure (host-tested). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Splits a *.SetConfig's params: built-in keys (and ro keys, dropped) into *builtin, each extension's keys into
 * ext_out[i] (NULL when it got none). false with err "unknown key <k>" for a key nobody owns. dry_run is skipped.
 * builtin, ro and each ext_keys[i] are NULL-terminated (builtin and ro may be NULL: none); ext_keys and ext_out
 * have n_ext entries (both may be NULL when n_ext is 0). *builtin_out is an object, empty when no built-in key came
 * (NULL only when it could not be made: the caller's print of it then fails as out of memory); an extension's object
 * that could not be made is false with err CFGEXT_NO_MEMORY. On false every output is NULL; on true the caller
 * deletes *builtin_out and each non-NULL ext_out[i]. A key both ro and an extension's is ro (dropped); one in builtin
 * and an extension's is built-in. */
#define CFGEXT_NO_MEMORY "no memory"
bool cfgext_split(const cJSON *params, const char *const *builtin, const char *const *ro,
                  const char *const *const *ext_keys, int n_ext,
                  cJSON **builtin_out, cJSON **ext_out, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
