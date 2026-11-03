#include "deckmodel/manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

#define MAX_CTLS 48

static void add_bitmap_preset(cJSON *presets, uint16_t w, uint16_t h, bool lcd)
{
    char name[24];
    snprintf(name, sizeof name, "%s_%ux%u", lcd ? "lcd" : "btn", (unsigned)w, (unsigned)h);
    if (cJSON_GetObjectItem(presets, name)) return;
    cJSON *p = cJSON_AddObjectToObject(presets, name);
    cJSON *bm = p ? cJSON_AddObjectToObject(p, "bitmap") : NULL;
    if (!bm) return;
    cJSON_AddNumberToObject(bm, "w", w);
    cJSON_AddNumberToObject(bm, "h", h);
}

char *deck_manifest_json(const deck_model_t *m)
{
    deck_ctl_t ctl[MAX_CTLS];
    int n = deck_model_controls(m, ctl, MAX_CTLS);
    if (n < 0) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *controls = root ? cJSON_AddObjectToObject(root, "controls") : NULL;
    cJSON *presets = root ? cJSON_AddObjectToObject(root, "stylePresets") : NULL;
    if (!controls || !presets) goto fail;

    cJSON_AddObjectToObject(presets, "default");
    cJSON_AddObjectToObject(presets, "empty");
    cJSON *rgb = cJSON_AddObjectToObject(presets, "rgb");
    if (rgb) cJSON_AddStringToObject(rgb, "colors", "hex");

    for (int i = 0; i < n; i++) {
        const deck_ctl_t *c = &ctl[i];
        char id[8], preset[24];
        deck_ctl_id(c, id);
        switch (c->kind) {
        case CTL_KEY_LCD:
            add_bitmap_preset(presets, c->w, c->h, false);
            snprintf(preset, sizeof preset, "btn_%ux%u", (unsigned)c->w, (unsigned)c->h);
            break;
        case CTL_LCD_CELL:
        case CTL_INFO:
            add_bitmap_preset(presets, c->w, c->h, true);
            snprintf(preset, sizeof preset, "lcd_%ux%u", (unsigned)c->w, (unsigned)c->h);
            break;
        case CTL_KEY_RGB:
            snprintf(preset, sizeof preset, "rgb");
            break;
        default:
            snprintf(preset, sizeof preset, "empty");
            break;
        }
        cJSON *e = cJSON_AddObjectToObject(controls, id);
        if (!e) goto fail;
        cJSON_AddNumberToObject(e, "row", c->row);
        cJSON_AddNumberToObject(e, "column", c->col);
        cJSON_AddStringToObject(e, "stylePreset", preset);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
fail:
    cJSON_Delete(root);
    return NULL;
}
