/* cfgext_split (cfgext_logic.h): pure, built by the host tests too. */
#include "cfgext_logic.h"

#include <stdio.h>
#include <string.h>

static bool in_list(const char *const *list, const char *key)
{
    for (; list && *list; list++)
        if (!strcmp(*list, key)) return true;
    return false;
}

static void clear(cJSON **builtin_out, cJSON **ext_out, int n_ext)
{
    cJSON_Delete(*builtin_out);
    *builtin_out = NULL;
    for (int i = 0; i < n_ext; i++) {
        cJSON_Delete(ext_out[i]);
        ext_out[i] = NULL;
    }
}

bool cfgext_split(const cJSON *params, const char *const *builtin, const char *const *ro,
                  const char *const *const *ext_keys, int n_ext,
                  cJSON **builtin_out, cJSON **ext_out, char *err, size_t errlen)
{
    for (int i = 0; i < n_ext; i++) ext_out[i] = NULL;
    *builtin_out = cJSON_CreateObject();
    const cJSON *it;
    cJSON_ArrayForEach(it, params) {
        if (!strcmp(it->string, "dry_run") || in_list(ro, it->string)) continue;
        cJSON **to = NULL;
        if (in_list(builtin, it->string)) {
            to = builtin_out;
        } else {
            for (int i = 0; i < n_ext && !to; i++)
                if (in_list(ext_keys[i], it->string)) {
                    if (!ext_out[i] && !(ext_out[i] = cJSON_CreateObject())) {
                        clear(builtin_out, ext_out, n_ext);
                        snprintf(err, errlen, CFGEXT_NO_MEMORY);
                        return false;
                    }
                    to = &ext_out[i];
                }
        }
        if (!to) {
            clear(builtin_out, ext_out, n_ext);
            snprintf(err, errlen, "unknown key %s", it->string);
            return false;
        }
        cJSON_AddItemToObject(*to, it->string, cJSON_Duplicate(it, true));
    }
    return true;
}
