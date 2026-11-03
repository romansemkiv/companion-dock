#pragma once
/* The problem list behind surfaces_problem_list (pure, host-tested; the caller provides locking):
 * newest first, no duplicates, at most SURF_PROBLEMS entries. A deck's quarantine text
 * (usbdeck/problem.h) is one entry per deck: a newer one replaces it, and it can be dropped by
 * serial when the quarantine ends. */
#include <stddef.h>

#include "usbdeck/problem.h"

#define SURF_PROBLEMS 8
#define SURF_PROBLEM_LEN USBDECK_PROBLEM_MAX /* usbdeck's texts fit whole */

typedef struct {
    char text[SURF_PROBLEMS][SURF_PROBLEM_LEN]; /* newest first */
    int n;
} surf_problems_t; /* zero-initialise */

/* msg (cut to SURF_PROBLEM_LEN - 1 bytes) becomes the newest entry. It replaces an equal entry and,
 * for a quarantine text, the same deck's quarantine text; else the oldest entry goes when the list
 * is full. NULL or "": nothing. */
void surf_problems_add(surf_problems_t *p, const char *msg);
/* a deck over the deck limit: "Too many Stream Decks (max <limit>): <serial> ignored" into out (n bytes);
 * returns its length (as snprintf). */
int surf_problem_too_many(char *out, size_t n, int limit, const char *serial);
/* Drops the quarantine text of the deck with this serial; the other entries keep their order.
 * Returns the number dropped. */
int surf_problems_drop_quarantine(surf_problems_t *p, const char *serial);
