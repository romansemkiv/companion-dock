/* Pure: QR codes for one key, on Nayuki's qrcodegen (MIT, qrcodegen/LICENSE). */
#include <string.h>

#include "deckui/deckui.h"
#include "qrcodegen/qrcodegen.h"

_Static_assert(DUI_QR_BUF == qrcodegen_BUFFER_LEN_FOR_VERSION(DUI_QR_VERSION_MAX), "DUI_QR_BUF");

bool dui_qr_encode(const char *payload, uint8_t qr[DUI_QR_BUF])
{
    uint8_t tmp[DUI_QR_BUF];
    if (!payload) return false;
    /* the smallest version (the largest modules on the key), at it ECC M when that fits, else L */
    for (int v = 1; v <= DUI_QR_VERSION_MAX; v++)
        if (qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_MEDIUM, v, v, qrcodegen_Mask_AUTO, false) ||
            qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_LOW, v, v, qrcodegen_Mask_AUTO, false))
            return true;
    return false;
}

int dui_qr_size(const uint8_t *qr) { return qrcodegen_getSize(qr); }

bool dui_qr_dark(const uint8_t *qr, int x, int y) { return qrcodegen_getModule(qr, x, y); }

int dui_qr_module_px(int key_px, int size) { return size > 0 ? key_px / (size + 2) : 0; }

/* Appends s to out (n bytes) at *u with \ ; , : " escaped (the WIFI: syntax). */
static void put_escaped(char *out, size_t n, int *u, const char *s)
{
    for (; s && *s; s++) {
        if (strchr("\\;,:\"", *s)) {
            if (out && (size_t)*u + 1 < n) out[*u] = '\\';
            (*u)++;
        }
        if (out && (size_t)*u + 1 < n) out[*u] = *s;
        (*u)++;
    }
}

static void put_str(char *out, size_t n, int *u, const char *s)
{
    for (; *s; s++, (*u)++)
        if (out && (size_t)*u + 1 < n) out[*u] = *s;
}

int dui_wifi_join(char *out, size_t n, const char *ssid, const char *pass)
{
    int u = 0;
    const bool open = !pass || !pass[0];
    put_str(out, n, &u, open ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:");
    put_escaped(out, n, &u, ssid);
    if (!open) {
        put_str(out, n, &u, ";P:");
        put_escaped(out, n, &u, pass);
    }
    put_str(out, n, &u, ";;");
    if (out && n) out[(size_t)u < n ? (size_t)u : n - 1] = '\0';
    return u;
}
