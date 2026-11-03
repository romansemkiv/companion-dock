/* Pure: the problem list (problems.h). */
#include "problems.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void remove_at(surf_problems_t *p, int i)
{
    memmove(p->text[i], p->text[i + 1], (size_t)(p->n - i - 1) * SURF_PROBLEM_LEN);
    p->n--;
}

/* Is entry e the quarantine text of the deck whose per-deck part is key[0..key_len)? */
static bool same_deck(const char *e, const char *key, size_t key_len)
{
    return key_len && usbdeck_quarantine_key_len(e) == key_len && strncmp(e, key, key_len) == 0;
}

void surf_problems_add(surf_problems_t *p, const char *msg)
{
    if (!msg || !msg[0]) return;
    char m[SURF_PROBLEM_LEN];
    snprintf(m, sizeof m, "%s", msg);
    const size_t key = usbdeck_quarantine_key_len(m);
    for (int i = p->n - 1; i >= 0; i--)
        if (strcmp(p->text[i], m) == 0 || same_deck(p->text[i], m, key)) remove_at(p, i);
    if (p->n == SURF_PROBLEMS) p->n--; /* the oldest goes */
    memmove(p->text[1], p->text[0], (size_t)p->n * SURF_PROBLEM_LEN);
    memcpy(p->text[0], m, sizeof m);
    p->n++;
}

int surf_problems_drop_quarantine(surf_problems_t *p, const char *serial)
{
    if (!serial || !serial[0]) return 0;
    char key[SURF_PROBLEM_LEN];
    const int n = snprintf(key, sizeof key, USBDECK_QUARANTINE_PREFIX, serial);
    if (n <= 0 || (size_t)n >= sizeof key || usbdeck_quarantine_key_len(key) != (size_t)n) return 0; /* no serial */
    int dropped = 0;
    for (int i = p->n - 1; i >= 0; i--) {
        if (same_deck(p->text[i], key, (size_t)n)) {
            remove_at(p, i);
            dropped++;
        }
    }
    return dropped;
}

int surf_problem_too_many(char *out, size_t n, int limit, const char *serial)
{
    return snprintf(out, n, "Too many Stream Decks (max %d): %s ignored", limit, serial ? serial : "?");
}
