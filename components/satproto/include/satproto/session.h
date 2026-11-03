#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int major, minor, patch;
    char companion_version[32];
    bool nonsquare, rotary_amount, leds;
} sat_caps_t;

typedef struct {
    const char *device_id, *control_id;
    const char *bitmap_b64; /* NULL when absent */
    size_t bitmap_b64_len;
    bool has_color;
    uint8_t r, g, b;
    bool pressed;
} sat_key_state_t;

typedef struct {
    void (*send)(void *ctx, const char *data, size_t len);
    void (*on_ready)(void *ctx, const sat_caps_t *caps);
    void (*on_too_old)(void *ctx, const char *api_version);
    void (*on_key_state)(void *ctx, const sat_key_state_t *ks);
    void (*on_clear)(void *ctx, const char *device_id);
    void (*on_brightness)(void *ctx, const char *device_id, int pct);
    void (*on_add_result)(void *ctx, const char *device_id, bool ok, const char *message);
} sat_session_cb_t;

typedef enum { SAT_S_WAIT_BEGIN, SAT_S_WAIT_CAPS, SAT_S_READY, SAT_S_TOO_OLD } sat_state_t;

typedef struct sat_session {
    sat_state_t state;
    sat_caps_t caps;
    uint32_t last_rx_ms, last_ping_ms;
    /* private */
    sat_session_cb_t cb;
    void *ctx;
    char *txbuf;
    size_t txcap;
} sat_session_t;

/* txbuf is caller-provided scratch used to format every outgoing line. A line
 * that does not fit is dropped, never sent partially. */
void sat_session_init(sat_session_t *s, const sat_session_cb_t *cb, void *ctx, char *txbuf, size_t txcap);
/* New connection: back to SAT_S_WAIT_BEGIN. */
void sat_session_reset(sat_session_t *s, uint32_t now_ms);
/* `line` is modified in place and has no trailing newline. */
void sat_session_on_line(sat_session_t *s, char *line, uint32_t now_ms);
/* Sends "PING \n" every 1000 ms once BEGIN has been received. */
void sat_session_tick(sat_session_t *s, uint32_t now_ms);
/* False after 5000 ms without any received line. */
bool sat_session_alive(const sat_session_t *s, uint32_t now_ms);

typedef struct {
    const char *device_id, *product_name, *manifest_json;
    bool brightness;
    const char *change_page_label; /* non-NULL: device can change page; sent as CAN_CHANGE_PAGE="<label>" */
} sat_device_desc_t;

/* False if the formatted line did not fit in txbuf (nothing is sent). */
bool sat_session_add_device(sat_session_t *s, const sat_device_desc_t *d);
void sat_session_remove_device(sat_session_t *s, const char *device_id);
void sat_session_key(sat_session_t *s, const char *device_id, const char *control_id, bool pressed);
/* A turn of delta steps (capped at 100 either way, as the reference client): one line with
 * DIRECTION=delta when Companion advertised ROTARY_AMOUNT, else |delta| lines of DIRECTION=+-1. */
void sat_session_rotate(sat_session_t *s, const char *device_id, const char *control_id, int delta);
void sat_session_change_page(sat_session_t *s, const char *device_id, bool forward);
