/* ESP-Hosted on the ESP32-C6: which firmware it runs against the one this P4
 * build carries, and what c6fw.c does about it. Pure (no ESP-IDF headers):
 * built by idf.py and by the host tests (test/host/test_c6fw_logic.c).
 * A "recovery firmware" here is a build without build-c6/ (no_image). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t major, minor, patch;
} c6ver_t;

/* True when the C6 must get the carried image: its version unknown (no
 * answer) or any other one, older or newer (a rolled-back P4 puts its own C6
 * firmware back). */
bool c6fw_needs_update(c6ver_t running, bool known, c6ver_t carried);

#define C6FW_MAX_TRIES 2 /* updates per power-on */

typedef enum {
    C6FW_OK,           /* the C6 runs the carried firmware */
    C6FW_UPDATE,       /* update it now (then the P4 restarts) */
    C6FW_WAIT_CONFIRM, /* this P4 image is not confirmed yet: update once it is
                        * (a restart now would make the bootloader take it for
                        * a failed start and roll back) */
    C6FW_GIVE_UP,      /* C6FW_MAX_TRIES updates since the power-on did not
                        * take: none more until the next power-on */
    C6FW_MISMATCH,     /* this build carries no image: it never updates */
} c6fw_action_t;

/* What to do at a start. pending: the running P4 image is not confirmed yet
 * (its first start after an OTA update); tries: updates since the last
 * power-on; no_image: this build carries no C6 image. */
c6fw_action_t c6fw_decide(c6ver_t running, bool known, c6ver_t carried, bool pending, int tries, bool no_image);

/* Wi-Fi with a C6 that is not on the carried firmware: only when it speaks the
 * same protocol (the same major version). */
bool c6fw_usable(c6ver_t running, bool known, c6ver_t carried);
