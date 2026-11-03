#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "netmgr_priv.h"

static const char *TAG = "netmgr";

#define DNS_PORT 53
#define DNS_HDR_LEN 12
#define DNS_MSG_MAX 512
#define DNS_NAME_MAX 255
#define DNS_LABEL_MAX 63
#define DNS_ANSWER_LEN 16
#define DNS_TTL_S 60
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1
#define DNS_FLAG_QR 0x8000
#define DNS_FLAG_AA 0x0400
#define DNS_FLAG_RD 0x0100

static TaskHandle_t s_task;
static esp_ip4_addr_t s_ip;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* Builds the reply to the query q[0..qlen) into out[0..cap). Returns the reply
 * length, or 0 when the packet must be ignored. Only the first question is
 * read; it is echoed back, with one A record pointing at ip when it asks for
 * A/IN and no records otherwise. Never reads q at or past qlen. */
static size_t dns_reply(const uint8_t *q, size_t qlen, uint8_t *out, size_t cap, uint32_t ip)
{
    if (qlen < DNS_HDR_LEN) return 0;
    uint16_t flags = rd16(q + 2);
    if (flags & DNS_FLAG_QR) return 0;      /* a response, not a query */
    if ((flags >> 11) & 0xF) return 0;      /* opcode other than QUERY */
    if (rd16(q + 4) == 0) return 0;         /* QDCOUNT */

    size_t pos = DNS_HDR_LEN, name_len = 1; /* the root label's zero byte */
    for (;;) {
        if (pos >= qlen) return 0;
        uint8_t l = q[pos];
        if (l == 0) break;
        if (l > DNS_LABEL_MAX) return 0;    /* compression pointer or reserved type */
        name_len += 1u + l;
        if (name_len > DNS_NAME_MAX) return 0;
        pos += 1u + l;
    }
    pos++;                                  /* root label */
    if (qlen - pos < 4) return 0;           /* pos <= qlen here */
    uint16_t qtype = rd16(q + pos), qclass = rd16(q + pos + 2);
    pos += 4;

    size_t question_len = pos - DNS_HDR_LEN;
    bool answer = qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN;
    size_t len = DNS_HDR_LEN + question_len + (answer ? DNS_ANSWER_LEN : 0);
    if (len > cap) return 0;

    memcpy(out, q, 2); /* ID */
    wr16(out + 2, DNS_FLAG_QR | DNS_FLAG_AA | (flags & DNS_FLAG_RD));
    wr16(out + 4, 1);
    wr16(out + 6, answer ? 1 : 0);
    wr16(out + 8, 0);
    wr16(out + 10, 0);
    memcpy(out + DNS_HDR_LEN, q + DNS_HDR_LEN, question_len);
    if (answer) {
        uint8_t *a = out + DNS_HDR_LEN + question_len;
        wr16(a, 0xC000 | DNS_HDR_LEN); /* name: pointer to the question */
        wr16(a + 2, DNS_TYPE_A);
        wr16(a + 4, DNS_CLASS_IN);
        wr16(a + 6, 0);
        wr16(a + 8, DNS_TTL_S);
        wr16(a + 10, 4);
        memcpy(a + 12, &ip, 4);
    }
    return len;
}

static void dns_task(void *arg)
{
    (void)arg;
    static uint8_t rx[DNS_MSG_MAX], tx[DNS_MSG_MAX];
    for (;;) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
            .sin_addr.s_addr = s_ip.addr,
        };
        if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
            ESP_LOGE(TAG, "captive dns: socket/bind failed (errno %d), retrying", errno);
            if (sock >= 0) close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(TAG, "captive dns: listening on " IPSTR ":%d", IP2STR(&s_ip), DNS_PORT);
        for (;;) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof from;
            int n = recvfrom(sock, rx, sizeof rx, 0, (struct sockaddr *)&from, &from_len);
            if (n < 0) {
                ESP_LOGW(TAG, "captive dns: recvfrom failed (errno %d)", errno);
                break;
            }
            size_t len = dns_reply(rx, (size_t)n, tx, sizeof tx, s_ip.addr);
            if (len) sendto(sock, tx, len, 0, (struct sockaddr *)&from, from_len);
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t captive_dns_start(uint32_t ip)
{
    if (s_task) return ESP_OK;
    s_ip.addr = ip;
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
