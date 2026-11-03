/* The status light's data pin (boxconfig/config.h). Pure: also built by the host tests.
 *
 * The Waveshare ESP32-P4-WIFI6-POE-ETH's free header pins are
 * 0-5, 7, 8, 20-23, 26, 27, 32, 33, 46-48. Of those this firmware uses GPIO7/8 (the I2C
 * bus to the speaker's ES8311 codec) and GPIO32 (the service button, svcbtn.h); every other GPIO is
 * wired to something on the board. */
#include <stddef.h>

#include "boxconfig/config.h"

#define GPIO_MAX 54

static const int FREE[] = {0, 1, 2, 3, 4, 5, 20, 21, 22, 23, 26, 27, 33, 46, 47, 48};

/* why each other GPIO can't carry the light */
static const char *const TAKEN[GPIO_MAX + 1] = {
    [6] = "GPIO6 is linked to the Wi-Fi chip on the board",
    [7] = "GPIO7 is the I2C bus to the speaker's codec",
    [8] = "GPIO8 is the I2C bus to the speaker's codec",
    [9] = "GPIO9 is the speaker's audio data (I2S)",
    [10] = "GPIO10 is the speaker's audio clock (I2S WS)",
    [11] = "GPIO11 is the speaker codec's audio input (I2S)",
    [12] = "GPIO12 is the speaker's audio clock (I2S BCLK)",
    [13] = "GPIO13 is the speaker codec's master clock",
    [14] = "GPIO14 is the Wi-Fi chip's SDIO bus",
    [15] = "GPIO15 is the Wi-Fi chip's SDIO bus",
    [16] = "GPIO16 is the Wi-Fi chip's SDIO bus",
    [17] = "GPIO17 is the Wi-Fi chip's SDIO bus",
    [18] = "GPIO18 is the Wi-Fi chip's SDIO bus",
    [19] = "GPIO19 is the Wi-Fi chip's SDIO bus",
    [24] = "GPIO24 is the chip's own USB port",
    [25] = "GPIO25 is the chip's own USB port",
    [28] = "GPIO28 is Ethernet (RMII)",
    [29] = "GPIO29 is Ethernet (RMII)",
    [30] = "GPIO30 is Ethernet (RMII)",
    [31] = "GPIO31 is Ethernet (RMII)",
    [32] = "GPIO32 is the service button",
    [34] = "GPIO34 is Ethernet (RMII)",
    [35] = "GPIO35 is Ethernet (RMII) and the BOOT button",
    [36] = "GPIO36 is a strapping pin",
    [37] = "GPIO37 is the serial console",
    [38] = "GPIO38 is the serial console",
    [39] = "GPIO39 is the microSD slot",
    [40] = "GPIO40 is the microSD slot",
    [41] = "GPIO41 is the microSD slot",
    [42] = "GPIO42 is the microSD slot",
    [43] = "GPIO43 is the microSD slot",
    [44] = "GPIO44 is the microSD slot",
    [45] = "GPIO45 is held low on the board",
    [49] = "GPIO49 is Ethernet (RMII)",
    [50] = "GPIO50 is Ethernet (RMII)",
    [51] = "GPIO51 is the Ethernet chip's reset",
    [52] = "GPIO52 is Ethernet (RMII)",
    [53] = "GPIO53 is the speaker amplifier's enable",
    [54] = "GPIO54 is the Wi-Fi chip's reset",
};

const char *boxcfg_led_pin_problem(int gpio)
{
    for (size_t i = 0; i < sizeof FREE / sizeof *FREE; i++)
        if (FREE[i] == gpio) return NULL;
    if (gpio >= 0 && gpio <= GPIO_MAX && TAKEN[gpio]) return TAKEN[gpio];
    return "not a free pin: use GPIO 0-5, 20-23, 26, 27, 33 or 46-48";
}

int boxcfg_led_pins(int *out, int max)
{
    int n = 0;
    for (size_t i = 0; i < sizeof FREE / sizeof *FREE && n < max; i++) out[n++] = FREE[i];
    return n;
}
