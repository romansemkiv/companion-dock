/* Inside boxconfig only. */
#pragma once

/* boxcfg_led_pin_problem without another component's claim (boxcfg_set_pin_claim): what a stored config is
 * loaded against. */
const char *boxcfg_pin_problem_base(int gpio);
