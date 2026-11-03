/* Pure: NotifyStatus's comparison key (notify_logic.h). */
#include "notify_logic.h"

#include <stdbool.h>

typedef struct {
    cJSON *parent, *item; /* item NULL: nothing taken out */
    int at;               /* its place among parent's items */
} taken_t;

/* Takes parent's first item named key (as cJSON_DeleteItemFromObjectCaseSensitive finds it) out of it. */
static void take(taken_t *t, cJSON *parent, const char *key)
{
    t->parent = parent;
    t->item = cJSON_GetObjectItemCaseSensitive(parent, key);
    t->at = 0;
    if (!t->item) return;
    for (const cJSON *c = parent->child; c && c != t->item; c = c->next) t->at++;
    cJSON_DetachItemViaPointer(parent, t->item);
}

/* ... and puts it back at its place (taken out last, put back first). */
static void put_back(const taken_t *t)
{
    if (t->item) cJSON_InsertItemInArray(t->parent, t->at, t->item); /* objects too: the item keeps its name */
}

char *notify_status_key(cJSON *st)
{
    if (!st) return NULL;
    taken_t t[3];
    take(&t[0], st, "uptime_s");
    take(&t[1], st, "heap");
    take(&t[2], cJSON_GetObjectItemCaseSensitive(st, "message"), "remaining_s");
    char *key = cJSON_PrintUnformatted(st);
    for (int i = 2; i >= 0; i--) put_back(&t[i]);
    return key;
}

uint32_t notify_fnv1a(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}
