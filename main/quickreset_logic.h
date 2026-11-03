/* Quick RESET presses (pure: no ESP-IDF, unit-tested on the host, test/host).
 * The board's BOOT button is an Ethernet line (GPIO35 = TXD1), so RESET is counted instead:
 * every start adds one to a count kept in NVS (main.c, namespace "qreset"), and being up
 * QUICKRESET_CLEAR_MS clears it; the start that makes it 3 turns the setup hotspot on for 15 minutes.
 * RESET pulls CHIP_PU, which the chip sees as a power-on: a press and a quick power cycle (a PoE
 * cable replugged) look the same, so nothing else is ever done by counting. */
#pragma once
#include <stdint.h>

#define QUICKRESET_CLEAR_MS 3000

typedef enum { QR_NONE, QR_SETUP } quickreset_action_t;

/* A start: the count goes up by one (saturating at 255, so it never comes round to 3 again);
 * QR_SETUP exactly when it reaches 3. */
quickreset_action_t quickreset_on_start(uint8_t *count);
