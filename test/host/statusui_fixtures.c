/* Test data: the status screens the box drew before the maintenance screens replaced them.
 * They are no longer firmware: they stay here as realistic content for the layout
 * engine's tests (zones, keep-with-next, values over keys, octets, dividers), which the message and the
 * maintenance screens' fitting still use. Built with the same rules render.c used. */
#include "statusui_fixtures.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void init(sui_content_t *c)
{
    memset(c, 0, sizeof *c);
    memset(c->fg, 255, 3);
}

static uint8_t role_letter(char ch)
{
    switch (ch) {
    case 'S': return SUI_ROLE_STATUS;
    case 'L': return SUI_ROLE_LABEL;
    case 'A': return SUI_ROLE_ACCENT;
    case 'V': return SUI_ROLE_VALUE;
    default: return SUI_ROLE_FG;
    }
}

static void add_line(sui_content_t *c, size_t *used, int group, const char *roles, const char *fmt, ...)
{
    if (c->n >= SUI_LINES) return;
    c->group[c->n] = (uint8_t)group;
    for (int w = 0, k = 0; w < SUI_LINE_ROLES; w++) {
        c->role[c->n][w] = role_letter(roles[k]);
        if (roles[k + 1]) k++;
    }
    size_t room = sizeof c->storage - *used;
    if (room < 1) return;
    char *dst = c->storage + *used;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (w < 0) {
        dst[0] = '\0';
        w = 0;
    }
    c->lines[c->n++] = dst;
    size_t adv = (size_t)w < room - 1 ? (size_t)w : room - 1;
    *used += adv + 1;
}

static const char *s_or_empty(const char *s) { return s ? s : ""; }

static void add_status(sui_content_t *c, size_t *used, int state, const char *text)
{
    c->state = (uint8_t)state;
    add_line(c, used, SUI_GROUP_STATUS, "S", "%s", text);
}

/* The service zone: the hotspot's name and password, whole (a deck never shows a wrong password),
 * kept together. An open hotspot (pass NULL or "") shows "No password" in the password's
 * place. Nothing without an SSID: the hotspot is off. */
static void add_hotspot(sui_content_t *c, size_t *used, const char *ssid, const char *pass)
{
    if (!ssid) return;
    const int n = c->n;
    add_line(c, used, SUI_GROUP_SERVICE, "LV", "WiFi %s", ssid);
    if (c->n > n) c->keep_next |= (uint8_t)(1u << n);
    if (pass && pass[0])
        add_line(c, used, SUI_GROUP_SERVICE, "LV", "Pass %s", pass);
    else
        add_line(c, used, SUI_GROUP_SERVICE, "L", "No password");
}

/* A label line that introduces the value line after it: they keep together. */
static void add_label(sui_content_t *c, size_t *used, const char *text)
{
    c->keep_next |= (uint8_t)(1u << c->n);
    add_line(c, used, SUI_GROUP_MAIN, "L", "%s", text);
}

void fx_screen_no_network(sui_content_t *c, const char *ap_ip, const char *ap_ssid, const char *ap_pass)
{
    size_t u = 0;
    init(c);
    add_status(c, &u, SUI_STATE_BAD, "No network");
    if (ap_ssid && ap_ip && ap_ip[0]) { /* the setup page over the hotspot, at its address */
        add_label(c, &u, "Setup page");
        add_line(c, &u, SUI_GROUP_MAIN, "A", "%s", ap_ip);
    } else {
        add_line(c, &u, SUI_GROUP_MAIN, "L", "Plug in network");
    }
    add_hotspot(c, &u, ap_ssid, ap_pass);
}

void fx_screen_looking(sui_content_t *c, const char *ip, const char *target,
                        const char *ap_ssid, const char *ap_pass)
{
    size_t u = 0;
    init(c);
    if (target) { /* the address it is trying: an IPv4 one in octets, in accent */
        add_status(c, &u, SUI_STATE_WAIT, "Connecting");
        add_line(c, &u, SUI_GROUP_MAIN, "V", "%s", target);
    } else {
        add_status(c, &u, SUI_STATE_WAIT, "Finding Companion");
    }
    add_label(c, &u, "Box IP");
    add_line(c, &u, SUI_GROUP_MAIN, "A", "%s", s_or_empty(ip));
    add_hotspot(c, &u, ap_ssid, ap_pass);
}

void fx_screen_choose(sui_content_t *c, const char *ip, const char *ap_ssid, const char *ap_pass)
{
    size_t u = 0;
    init(c);
    add_status(c, &u, SUI_STATE_WAIT, "Choose Companion");
    add_label(c, &u, "Web page");
    add_line(c, &u, SUI_GROUP_MAIN, "A", "%s", s_or_empty(ip)); /* its octets on keys of their own ("http://" made it one word) */
    add_hotspot(c, &u, ap_ssid, ap_pass);
}

void fx_screen_too_old(sui_content_t *c, const char *ver, const char *ap_ssid, const char *ap_pass)
{
    size_t u = 0;
    init(c);
    add_status(c, &u, SUI_STATE_BAD, "Companion too old");
    add_line(c, &u, SUI_GROUP_MAIN, "LV", "Needs 5.0+");
    add_line(c, &u, SUI_GROUP_MAIN, "LV", "Has %s", s_or_empty(ver));
    add_hotspot(c, &u, ap_ssid, ap_pass);
}

void fx_screen_no_dock(sui_content_t *c)
{
    size_t u = 0;
    init(c);
    add_status(c, &u, SUI_STATE_BAD, "No free dock");
    add_line(c, &u, SUI_GROUP_MAIN, "L", "Max 6 decks");
}

void fx_screen_dock(sui_content_t *c, const char *ip, uint16_t port, const char *ap_ssid, const char *ap_pass)
{
    size_t u = 0;
    init(c);
    add_status(c, &u, SUI_STATE_WAIT, "Waiting for Companion");
    add_label(c, &u, "Remote surface"); /* what to add in Companion */
    add_line(c, &u, SUI_GROUP_MAIN, ip && ip[0] ? "ALA" : "LA", "%.16s port %u", s_or_empty(ip), (unsigned)port);
    add_hotspot(c, &u, ap_ssid, ap_pass);
}
