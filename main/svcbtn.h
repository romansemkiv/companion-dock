/* The service key: a push button from its pin (keyPin, GPIO32 by default) to GND, internal pull-up, active
 * low; a NeoKey (Adafruit 5756) wired A to the pin and C to GND among them. A short press does keyShort, a long
 * one (held 3 s, let go before 5 s) keyLong: the box menu, Find my box, sound on / off, light on / off, restart (long
 * only) or nothing; held 5 s then released: the setup hotspot for 15 minutes; held 10 s then released: factory reset
 * and restart (svcbtn_logic.h). keyEnabled off: nothing at all. A press clicks and flashes the light (as a deck key
 * does, by the sound and light settings); 3 s, 5 s and 10 s each confirm. */
#pragma once
#include "boxconfig/config.h"
#include "esp_err.h"

/* The key's settings (keyEnabled, keyPin, keyShort) copied from cfg: a new pin is taken by the polling task at
 * its next poll. Any task, never blocks; call it before svcbtn_start and at every settings change. */
void svcbtn_apply(const box_config_t *cfg);

/* Starts the polling task. Call after netmgr_start. */
esp_err_t svcbtn_start(void);
