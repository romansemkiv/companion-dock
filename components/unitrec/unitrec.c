#include "unitrec.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "unitrec";

/* written once in app_main, before any task that reads them starts */
static bool s_loaded;
static unit_status_t s_st = UNIT_NONE;
static unit_t s_unit;

void unitrec_load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    /* the MAC the box id is made of (rpc.c); on the P4 (one universal MAC) it is the chip's base MAC,
     * the one esptool reads */
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_ETH);
    char board[13];
    snprintf(board, sizeof board, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    uint8_t *sec = malloc(UNIT_SECTOR_SIZE);
    if (!sec) {
        ESP_LOGW(TAG, "unit record not read: no memory");
        return;
    }
    /* outside every partition, inside the flash size the image is built for (16 MB) */
    const esp_err_t e = esp_flash_read(NULL, sec, UNIT_ADDR, UNIT_SECTOR_SIZE);
    if (e == ESP_OK) s_st = unit_parse(sec, UNIT_SECTOR_SIZE, board, &s_unit);
    free(sec);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "unit record not read: %s", esp_err_to_name(e));
    } else if (s_st == UNIT_OK) {
        ESP_LOGI(TAG, "unit record: case SN %s, case type %s, %s, written %s", s_unit.case_sn, s_unit.case_type,
                 s_unit.edition, s_unit.at);
    } else if (s_st == UNIT_OTHER_BOARD) {
        ESP_LOGW(TAG, "unit record: written for another board, not %s", board);
    } else if (s_st == UNIT_UNREADABLE) {
        ESP_LOGW(TAG, "unit record: unreadable");
    }
}

unit_status_t unitrec_get(unit_t *out)
{
    *out = s_unit;
    return s_st;
}
