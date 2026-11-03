/* The optional service button: a push button from GPIO32 to GND (internal pull-up, active low).
 * Held 5 s then released: the setup hotspot for 15 minutes; held 10 s then
 * released: factory reset and restart (svcbtn_logic.h). */
#pragma once
#include "esp_err.h"

#define SVCBTN_GPIO 32

/* Configures the pin and starts the polling task. Call after netmgr_start. */
esp_err_t svcbtn_start(void);
