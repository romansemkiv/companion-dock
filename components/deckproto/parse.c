/* Pure: parses output (image) reports, the inverse of the writers in images.c. The layouts are the
 * ones the Stream Deck host library writes too (@elgato-stream-deck/core 7.8.0,
 * services/imageWriter/headerGenerator.js), so Companion's reports parse the same way. */
#include "deckproto/deckproto.h"
#include "report_hdr.h"

uint16_t deckproto_image_first_chunk(const deck_model_t *m) { return m && m->gen == DECK_GEN1_ORIG ? 1 : 0; }

/* gen2 payload: its length is in the header and must fit the report as given and the model's. */
static bool gen2_payload(const deck_model_t *m, const uint8_t *r, size_t len, size_t hdr, uint16_t plen,
                         const uint8_t **payload, size_t *payload_len)
{
    if (plen == 0 || hdr + plen > len || hdr + plen > m->report_len) return false;
    *payload = r + hdr;
    *payload_len = plen;
    return true;
}

bool deckproto_parse_image_report(const deck_model_t *m, const uint8_t *r, size_t len, deck_img_target_t *t,
                                  uint16_t *chunk_index, bool *last, const uint8_t **payload, size_t *payload_len)
{
    if (!m || !r || len < 2 || r[0] != 0x02 || m->pedal || m->fmt == DECK_IMG_NONE) return false;
    const unsigned nkeys = (unsigned)m->cols * m->rows;
    deck_img_target_t tg = {0};

    if (m->gen != DECK_GEN2) { /* 02 01 page 00 last key+1, 16-byte header, no payload length */
        if (r[1] != 0x01 || len <= GEN1_HDR || r[5] == 0 || r[5] > nkeys) return false;
        tg.kind = DECK_TGT_KEY;
        tg.key = (uint8_t)(r[5] - 1);
        *t = tg;
        *chunk_index = r[2];
        *last = r[4] != 0;
        *payload = r + GEN1_HDR;
        *payload_len = (len < m->report_len ? len : m->report_len) - GEN1_HDR;
        return true;
    }

    switch (r[1]) {
    case 0x07: /* key: 02 07 key last len16 page16 */
        if (len < GEN2_KEY_HDR || r[2] >= nkeys) return false;
        tg.kind = DECK_TGT_KEY;
        tg.key = r[2];
        if (!gen2_payload(m, r, len, GEN2_KEY_HDR, rd16(r + 4), payload, payload_len)) return false;
        *last = r[3] != 0;
        *chunk_index = rd16(r + 6);
        break;
    case 0x0B: /* Neo info bar: 02 0b 00 last len16 page16 */
        if (m->info_w == 0 || len < GEN2_KEY_HDR) return false;
        tg.kind = DECK_TGT_INFO;
        if (!gen2_payload(m, r, len, GEN2_KEY_HDR, rd16(r + 4), payload, payload_len)) return false;
        *last = r[3] != 0;
        *chunk_index = rd16(r + 6);
        break;
    case 0x0C: /* LCD region: 02 0c x16 y16 w16 h16 last page16 len16 */
        if (m->lcd_w == 0 || len < GEN2_LCD_HDR) return false;
        tg.kind = DECK_TGT_LCD;
        tg.x = rd16(r + 2);
        tg.y = rd16(r + 4);
        tg.w = rd16(r + 6);
        tg.h = rd16(r + 8);
        if (!gen2_payload(m, r, len, GEN2_LCD_HDR, rd16(r + 13), payload, payload_len)) return false;
        *last = r[10] != 0;
        *chunk_index = rd16(r + 11);
        break;
    default: return false;
    }
    *t = tg;
    return true;
}
