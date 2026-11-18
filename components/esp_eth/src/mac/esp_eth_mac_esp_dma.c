/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_cache.h"
#include "hal/cache_ll.h" /* Companion Dock patch: the non-cacheable alias */
#include "hal/emac_hal.h"
#include "esp_heap_caps.h"
#include "esp_private/eth_mac_esp_dma.h"

#define ETH_CRC_LENGTH (4)

#define EMAC_ALLOC_BUF_MAGIC_ID 0x1E1C8416

#define EMAC_TDES0_FS_CTRL_FLAGS_MASK 0x0FCC0000 // modifiable bits mask associated with the First Segment
#define EMAC_TDES0_LS_CTRL_FLAGS_MASK 0x40000000 // modifiable bits mask associated with the Last Segment

#define PTP_TX_TIMESTAMP_TO 50 //  maximum loops observed on P4 was 31 @ETH frame 1500B

#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#define DMA_CACHE_WB(addr, size) do {                                                           \
    esp_err_t msync_ret = esp_cache_msync((void *)addr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);    \
    assert(msync_ret == ESP_OK);                                                                \
    } while(0)
#else
#define DMA_CACHE_WB(addr, size)
#endif

#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#define DMA_CACHE_INVALIDATE(addr, size) do {                                                   \
    esp_err_t msync_ret = esp_cache_msync((void *)addr, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);    \
    assert(msync_ret == ESP_OK);                                                                \
    } while(0)
#else
#define DMA_CACHE_INVALIDATE(addr, size)
#endif

static const char *TAG = "esp.emac.dma";

_Static_assert(EMAC_ESP_DMA_HDR_LEN <= CONFIG_ETH_DMA_BUFFER_SIZE, "a frame's header is read from its first DMA buffer");

struct emac_esp_dma_t {
    emac_hal_context_t hal;
    uint32_t tx_desc_flags;
    uint32_t rx_desc_flags;
    void *descriptors;
    eth_dma_rx_descriptor_t *rx_desc;
    eth_dma_tx_descriptor_t *tx_desc;
    uint8_t *rx_buf[CONFIG_ETH_DMA_RX_BUFFER_NUM];
    uint8_t *tx_buf[CONFIG_ETH_DMA_TX_BUFFER_NUM];
};

typedef struct {
#ifndef NDEBUG
    uint32_t magic_id;
#endif // NDEBUG
    uint32_t copy_len;
} __attribute__((packed)) emac_esp_dma_auto_buf_info_t;

void emac_esp_dma_reset(emac_esp_dma_handle_t emac_esp_dma)
{
    /* reset DMA descriptors */
    emac_esp_dma->rx_desc = (eth_dma_rx_descriptor_t *)(emac_esp_dma->descriptors);
    emac_esp_dma->tx_desc = (eth_dma_tx_descriptor_t *)(emac_esp_dma->descriptors +
                                                        sizeof(eth_dma_rx_descriptor_t) * CONFIG_ETH_DMA_RX_BUFFER_NUM);
    /* init rx chain */
    for (int i = 0; i < CONFIG_ETH_DMA_RX_BUFFER_NUM; i++) {
        /* Set Own bit of the Rx descriptor Status: DMA */
        emac_esp_dma->rx_desc[i].RDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
        /* Set Buffer1 size and Second Address Chained bit */
        emac_esp_dma->rx_desc[i].RDES1.SecondAddressChained = 1;
        emac_esp_dma->rx_desc[i].RDES1.ReceiveBuffer1Size = CONFIG_ETH_DMA_BUFFER_SIZE;
        /* Enable Ethernet DMA Rx Descriptor interrupt */
        emac_esp_dma->rx_desc[i].RDES1.DisableInterruptOnComplete = 0;
        /* point to the buffer */
        emac_esp_dma->rx_desc[i].Buffer1Addr = (uint32_t)(emac_esp_dma->rx_buf[i]);
        /* point to next descriptor */
        emac_esp_dma->rx_desc[i].Buffer2NextDescAddr = (uint32_t)(emac_esp_dma->rx_desc + i + 1);

        /* For last descriptor, set next descriptor address register equal to the first descriptor base address */
        if (i == CONFIG_ETH_DMA_RX_BUFFER_NUM - 1) {
            emac_esp_dma->rx_desc[i].Buffer2NextDescAddr = (uint32_t)(emac_esp_dma->rx_desc);
        }
        DMA_CACHE_WB(&emac_esp_dma->rx_desc[i], EMAC_HAL_DMA_DESC_SIZE);
    }

    /* init tx chain */
    for (int i = 0; i < CONFIG_ETH_DMA_TX_BUFFER_NUM; i++) {
        /* Set Own bit of the Tx descriptor Status: CPU */
        emac_esp_dma->tx_desc[i].TDES0.Own = EMAC_LL_DMADESC_OWNER_CPU;
        emac_esp_dma->tx_desc[i].TDES0.SecondAddressChained = 1;
        emac_esp_dma->tx_desc[i].TDES1.TransmitBuffer1Size = CONFIG_ETH_DMA_BUFFER_SIZE;
        /* point to the buffer */
        emac_esp_dma->tx_desc[i].Buffer1Addr = (uint32_t)(emac_esp_dma->tx_buf[i]);
        /* point to next descriptor */
        emac_esp_dma->tx_desc[i].Buffer2NextDescAddr = (uint32_t)(emac_esp_dma->tx_desc + i + 1);

        /* For last descriptor, set next descriptor address register equal to the first descriptor base address */
        if (i == CONFIG_ETH_DMA_TX_BUFFER_NUM - 1) {
            emac_esp_dma->tx_desc[i].Buffer2NextDescAddr = (uint32_t)(emac_esp_dma->tx_desc);
        }
        DMA_CACHE_WB(&emac_esp_dma->tx_desc[i], EMAC_HAL_DMA_DESC_SIZE);
    }

    /* set base address of the first descriptor */
    emac_hal_set_rx_tx_desc_addr(&emac_esp_dma->hal, emac_esp_dma->rx_desc, emac_esp_dma->tx_desc);
}

void emac_esp_dma_set_tdes0_ctrl_bits(emac_esp_dma_handle_t emac_esp_dma, uint32_t flag)
{
    emac_esp_dma->tx_desc_flags |= flag;
}

void emac_esp_dma_clear_tdes0_ctrl_bits(emac_esp_dma_handle_t emac_esp_dma, uint32_t flag)
{
    emac_esp_dma->tx_desc_flags &= ~flag;
}

void emac_esp_dma_ts_enable(emac_esp_dma_handle_t emac_esp_dma, bool enable)
{
    if (enable) {
        emac_esp_dma_set_tdes0_ctrl_bits(emac_esp_dma, EMAC_HAL_TDES0_TX_TS_ENABLE);
    } else {
        emac_esp_dma_clear_tdes0_ctrl_bits(emac_esp_dma, EMAC_HAL_TDES0_TX_TS_ENABLE);
    }
}

uint32_t emac_esp_dma_transmit_frame(emac_esp_dma_handle_t emac_esp_dma, uint8_t *buf, uint32_t length)
{
    /* Get the number of Tx buffers to use for the frame */
    uint32_t bufcount = 0;
    uint32_t lastlen = length;
    uint32_t sentout = 0;
    while (lastlen > CONFIG_ETH_DMA_BUFFER_SIZE) {
        lastlen -= CONFIG_ETH_DMA_BUFFER_SIZE;
        bufcount++;
    }
    if (lastlen) {
        bufcount++;
    }
    if (bufcount > CONFIG_ETH_DMA_TX_BUFFER_NUM) {
        goto err;
    }

    eth_dma_tx_descriptor_t *desc_iter = emac_esp_dma->tx_desc;
    /* A frame is transmitted in multiple descriptor */
    for (size_t i = 0; i < bufcount; i++) {
        DMA_CACHE_INVALIDATE(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
        /* Check if the descriptor is owned by the Ethernet DMA (when 1) or CPU (when 0) */
        if (desc_iter->TDES0.Own != EMAC_LL_DMADESC_OWNER_CPU) {
            goto err;
        }
        /* Clear FIRST and LAST segment bits */
        desc_iter->TDES0.FirstSegment = 0;
        desc_iter->TDES0.LastSegment = 0;
        desc_iter->TDES0.Value &= ~(EMAC_TDES0_FS_CTRL_FLAGS_MASK | EMAC_TDES0_LS_CTRL_FLAGS_MASK);
        if (i == 0) {
            /* Setting the first segment bit */
            desc_iter->TDES0.FirstSegment = 1;
            desc_iter->TDES0.Value |= emac_esp_dma->tx_desc_flags & EMAC_TDES0_FS_CTRL_FLAGS_MASK;
        }
        if (i == (bufcount - 1)) {
            /* Setting the last segment bit */
            desc_iter->TDES0.LastSegment = 1;
            desc_iter->TDES0.Value |= emac_esp_dma->tx_desc_flags & EMAC_TDES0_LS_CTRL_FLAGS_MASK;
            /* Program size */
            desc_iter->TDES1.TransmitBuffer1Size = lastlen;
            /* copy data from uplayer stack buffer */
            memcpy((void *)(desc_iter->Buffer1Addr), buf + i * CONFIG_ETH_DMA_BUFFER_SIZE, lastlen);
            sentout += lastlen;
        } else {
            /* Program size */
            desc_iter->TDES1.TransmitBuffer1Size = CONFIG_ETH_DMA_BUFFER_SIZE;
            /* copy data from uplayer stack buffer */
            memcpy((void *)(desc_iter->Buffer1Addr), buf + i * CONFIG_ETH_DMA_BUFFER_SIZE, CONFIG_ETH_DMA_BUFFER_SIZE);
            sentout += CONFIG_ETH_DMA_BUFFER_SIZE;
        }
        DMA_CACHE_WB(desc_iter->Buffer1Addr, CONFIG_ETH_DMA_BUFFER_SIZE);
        /* Point to next descriptor */
        desc_iter = (eth_dma_tx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
    }

    /* Set Own bit of the Tx descriptor Status: gives the buffer back to ETHERNET DMA */
    for (size_t i = 0; i < bufcount; i++) {
        emac_esp_dma->tx_desc->TDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
        DMA_CACHE_WB(emac_esp_dma->tx_desc, EMAC_HAL_DMA_DESC_SIZE);
        emac_esp_dma->tx_desc = (eth_dma_tx_descriptor_t *)(emac_esp_dma->tx_desc->Buffer2NextDescAddr);
    }
    emac_hal_transmit_poll_demand(&emac_esp_dma->hal);
    return sentout;
err:
    return 0;
}

uint32_t emac_esp_dma_transmit_frame_ext(emac_esp_dma_handle_t emac_esp_dma, emac_esp_dma_transmit_buff_t *buffs_array, uint32_t buffs_cnt, eth_mac_time_t *ts)
{
    /* Get the number of Tx buffers to use for the frame */
    uint32_t dma_bufcount = 0;
    uint32_t sentout = 0;
    uint32_t avail_len = CONFIG_ETH_DMA_BUFFER_SIZE;
    uint8_t *ptr = buffs_array->buf;
    uint32_t lastlen = buffs_array->size;

    eth_dma_tx_descriptor_t *desc_iter = emac_esp_dma->tx_desc;
#if SOC_EMAC_IEEE1588V2_SUPPORTED
    eth_dma_tx_descriptor_t *desc_last = desc_iter;
#endif
    /* A frame is transmitted in multiple descriptor */
    while (dma_bufcount < CONFIG_ETH_DMA_TX_BUFFER_NUM) {
        DMA_CACHE_INVALIDATE(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
        /* Check if the descriptor is owned by the Ethernet DMA (when 1) or CPU (when 0) */
        if (desc_iter->TDES0.Own != EMAC_LL_DMADESC_OWNER_CPU) {
            goto err;
        }
        /* Clear FIRST and LAST segment bits */
        desc_iter->TDES0.FirstSegment = 0;
        desc_iter->TDES0.LastSegment = 0;
        desc_iter->TDES0.Value &= ~(EMAC_TDES0_FS_CTRL_FLAGS_MASK | EMAC_TDES0_LS_CTRL_FLAGS_MASK);
        desc_iter->TDES1.TransmitBuffer1Size = 0;
        if (dma_bufcount == 0) {
            /* Setting the first segment bit */
            desc_iter->TDES0.FirstSegment = 1;
            desc_iter->TDES0.Value |= emac_esp_dma->tx_desc_flags & EMAC_TDES0_FS_CTRL_FLAGS_MASK;
        }

        while (buffs_cnt > 0) {
            /* Check if input buff data fits to currently available space in the descriptor */
            if (lastlen < avail_len) {
                /* copy data from uplayer stack buffer */
                memcpy((void *)(desc_iter->Buffer1Addr + (CONFIG_ETH_DMA_BUFFER_SIZE - avail_len)), ptr, lastlen);
                sentout += lastlen;
                avail_len -= lastlen;
                desc_iter->TDES1.TransmitBuffer1Size += lastlen;

                /* Update processed input buffers info */
                buffs_cnt--;
                buffs_array++;
                ptr = buffs_array->buf;
                lastlen = buffs_array->size;
                /* There is only limited available space in the current descriptor, use it all */
            } else {
                /* copy data from uplayer stack buffer */
                memcpy((void *)(desc_iter->Buffer1Addr + (CONFIG_ETH_DMA_BUFFER_SIZE - avail_len)), ptr, avail_len);
                sentout += avail_len;
                lastlen -= avail_len;
                /* If lastlen is not zero, input buff will be fragmented over multiple descriptors */
                if (lastlen > 0) {
                    ptr += avail_len;
                    /* Input buff fully fits the descriptor, move to the next input buff */
                } else {
                    /* Update processed input buffers info */
                    buffs_cnt--;
                    buffs_array++;
                    ptr = buffs_array->buf;
                    lastlen = buffs_array->size;
                }
                avail_len = CONFIG_ETH_DMA_BUFFER_SIZE;
                desc_iter->TDES1.TransmitBuffer1Size = CONFIG_ETH_DMA_BUFFER_SIZE;
                /* The descriptor is full here so exit and use the next descriptor */
                break;
            }
        }
        DMA_CACHE_WB(desc_iter->Buffer1Addr, CONFIG_ETH_DMA_BUFFER_SIZE);
        /* Increase counter of utilized DMA buffers */
        dma_bufcount++;

        /* If all input buffers processed, mark as LAST segment and finish the coping */
        if (buffs_cnt == 0) {
            /* Setting the last segment bit */
            desc_iter->TDES0.LastSegment = 1;
            desc_iter->TDES0.Value |= emac_esp_dma->tx_desc_flags & EMAC_TDES0_LS_CTRL_FLAGS_MASK;
#if SOC_EMAC_IEEE1588V2_SUPPORTED
            desc_last = desc_iter;
#endif
            break;
        }

        /* Point to next descriptor */
        desc_iter = (eth_dma_tx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
    }

    /* Set Own bit of the Tx descriptor Status: gives the buffer back to ETHERNET DMA */
    for (size_t i = 0; i < dma_bufcount; i++) {
        emac_esp_dma->tx_desc->TDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
        DMA_CACHE_WB(emac_esp_dma->tx_desc, EMAC_HAL_DMA_DESC_SIZE);
        emac_esp_dma->tx_desc = (eth_dma_tx_descriptor_t *)(emac_esp_dma->tx_desc->Buffer2NextDescAddr);
    }

    emac_hal_transmit_poll_demand(&emac_esp_dma->hal);

#if SOC_EMAC_IEEE1588V2_SUPPORTED
    if (ts != NULL) {
        uint32_t timeout = 0;
        do {
            timeout++;
            DMA_CACHE_INVALIDATE(desc_last, EMAC_HAL_DMA_DESC_SIZE);
        } while (emac_hal_get_txdesc_timestamp(&emac_esp_dma->hal, desc_last, &ts->seconds, &ts->nanoseconds) == ESP_ERR_INVALID_STATE &&
                 timeout < PTP_TX_TIMESTAMP_TO);
        if (timeout >= PTP_TX_TIMESTAMP_TO) {
            /* zeros indicate invalid time stamp since it is not possible to ever get "zero time" under normal conditions */
            ts->seconds = 0;
            ts->nanoseconds = 0;
        }
    }
#endif

    return sentout;
err:
    return 0;
}

static esp_err_t emac_esp_dma_get_valid_recv_len(emac_esp_dma_handle_t emac_esp_dma, uint32_t *ret_len)
{
    eth_dma_rx_descriptor_t *desc_iter = emac_esp_dma->rx_desc;
    uint32_t used_descs = 0;
    DMA_CACHE_INVALIDATE(desc_iter, EMAC_HAL_DMA_DESC_SIZE);

    /* Traverse descriptors owned by CPU */
    while ((desc_iter->RDES0.Own == EMAC_LL_DMADESC_OWNER_CPU) && (used_descs < CONFIG_ETH_DMA_RX_BUFFER_NUM)) {
        used_descs++;
        /* Last segment in frame */
        if (desc_iter->RDES0.LastDescriptor) {
            /* Since Store Forward must be disabled on some targets, DMA descriptors may contain erroneous frames */
            /* In addition, "Descriptor Error" (no free descriptors) may truncate a frame even if Store Forward is enabled */
            if (desc_iter->RDES0.ErrSummary) {
                emac_esp_dma_flush_recv_frame(emac_esp_dma);
                *ret_len = 0;
                return ESP_FAIL;
            }
            /* Get the Frame Length of the received packet: substruct 4 bytes of the CRC */
            *ret_len = desc_iter->RDES0.FrameLength - ETH_CRC_LENGTH;
            break;
        }
        /* First segment in frame */
        if (desc_iter->RDES0.FirstDescriptor) {
            emac_esp_dma->rx_desc = desc_iter;
        }
        /* point to next descriptor */
        desc_iter = (eth_dma_rx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
        DMA_CACHE_INVALIDATE(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
    }

    return ESP_OK;
}

/* Companion Dock patch: see eth_mac_esp_dma.h. The descriptors and the RX buffers are internal RAM (L2MEM): their
 * non-cacheable alias reads what the DMA wrote and writes where the DMA reads, so the drop path needs no cache sync
 * (four a frame before, most of the cost). ESP-IDF's cached path, which takes the kept frames, invalidates before it
 * reads and writes back what it writes, so the two views never hold a stale line over each other. */
#define NC_DESC(d) ((volatile eth_dma_rx_descriptor_t *)CACHE_LL_L2MEM_NON_CACHE_ADDR(d))
#define RDES0_OWN_DMA (1u << 31)
#define RDES0_ERR_SUMMARY (1u << 15)
#define RDES0_LAST (1u << 8)
#define RDES0_LEN(w) (((w) >> 16) & 0x3fff)

/* The ring never moves (emac_esp_dma_reset chains descriptor i to i + 1 and the last to the first, with buffer i):
 * the next descriptor and a descriptor's buffer come from its index, not from uncached reads of its pointers. */
static inline eth_dma_rx_descriptor_t *rx_next(emac_esp_dma_handle_t emac_esp_dma, eth_dma_rx_descriptor_t *d)
{
    eth_dma_rx_descriptor_t *base = (eth_dma_rx_descriptor_t *)emac_esp_dma->descriptors;
    return d + 1 == base + CONFIG_ETH_DMA_RX_BUFFER_NUM ? base : d + 1;
}

static inline const uint8_t *rx_buf_of(emac_esp_dma_handle_t emac_esp_dma, const eth_dma_rx_descriptor_t *d)
{
    return emac_esp_dma->rx_buf[d - (const eth_dma_rx_descriptor_t *)emac_esp_dma->descriptors];
}

/* the descriptors from d on (a frame's n) back to the DMA; the head moves past them */
static void rx_give_back(emac_esp_dma_handle_t emac_esp_dma, eth_dma_rx_descriptor_t *d, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        NC_DESC(d)->RDES0.Value = RDES0_OWN_DMA;
        d = rx_next(emac_esp_dma, d);
    }
    emac_esp_dma->rx_desc = d;
}

/* How much of a frame the filter looks at, in words read uncached (each a bus round trip, most of a frame's cost):
 * a unicast frame's first 16 bytes (its destination says it is unicast); a broadcast or multicast one's first 44
 * (Ethernet, IPv4 and UDP headers, or an ARP packet), 64 with IPv4 options. */
static uint32_t rx_read_hdr(const volatile uint32_t *src, uint32_t len, uint32_t *hdr)
{
    uint32_t got = 4;
    for (uint32_t i = 0; i < got; i++) {
        hdr[i] = src[i];
    }
    const uint8_t *b = (const uint8_t *)hdr;
    uint32_t want = 16;
    if (b[0] & 1) {
        want = (b[12] == 0x08 && b[13] == 0x00 && (b[14] & 0x0f) > 5) ? EMAC_ESP_DMA_HDR_LEN : 44;
    }
    if (want > len) {
        want = len;
    }
    for (uint32_t i = got; i < (want + 3) / 4; i++) {
        hdr[i] = src[i];
    }
    return want;
}

bool emac_esp_dma_rx_filter(emac_esp_dma_handle_t emac_esp_dma,
                            bool (*keep)(const uint8_t *hdr, uint32_t hdr_len, uint32_t len, void *ctx), void *ctx,
                            uint32_t *count, uint32_t budget)
{
    bool waiting = false;
    uint32_t n_dropped = 0;
    while (*count + n_dropped < budget) {
        eth_dma_rx_descriptor_t *first = emac_esp_dma->rx_desc, *d = first;
        uint32_t w = NC_DESC(d)->RDES0.Value, n = 1;
        if (w & RDES0_OWN_DMA) {
            break; /* nothing received */
        }
        while (!(w & RDES0_LAST) && n < CONFIG_ETH_DMA_RX_BUFFER_NUM) { /* the frame's other descriptors */
            d = rx_next(emac_esp_dma, d);
            w = NC_DESC(d)->RDES0.Value;
            if (w & RDES0_OWN_DMA) {
                goto out; /* still arriving */
            }
            n++;
        }
        __asm__ __volatile__("fence r, r" ::: "memory"); /* the status before the data it covers */
        const uint32_t len = RDES0_LEN(w) > ETH_CRC_LENGTH ? RDES0_LEN(w) - ETH_CRC_LENGTH : 0;
        if ((w & RDES0_ERR_SUMMARY) || !(w & RDES0_LAST) || len == 0) {
            rx_give_back(emac_esp_dma, first, n);
            n_dropped++;
            continue;
        }
        uint32_t hdr[EMAC_ESP_DMA_HDR_LEN / 4];
        const volatile uint32_t *src = (const volatile uint32_t *)CACHE_LL_L2MEM_NON_CACHE_ADDR(rx_buf_of(emac_esp_dma, first));
        const uint32_t hl = rx_read_hdr(src, len, hdr);
        if (keep((const uint8_t *)hdr, hl, len, ctx)) {
            waiting = true;
            break;
        }
        rx_give_back(emac_esp_dma, first, n);
        n_dropped++;
    }
out:
    if (n_dropped) {
        __asm__ __volatile__("fence w, o" ::: "memory"); /* the descriptors given back before the DMA looks */
        emac_hal_receive_poll_demand(&emac_esp_dma->hal); /* a DMA stopped on a full ring goes on */
    }
    *count += n_dropped;
    return waiting;
}

void emac_esp_dma_get_remain_frames(emac_esp_dma_handle_t emac_esp_dma, uint32_t *remain_frames, uint32_t *free_descs)
{
    eth_dma_rx_descriptor_t *desc_iter = emac_esp_dma->rx_desc;
    *remain_frames = 0;
    uint32_t used_descs = 0;

    DMA_CACHE_INVALIDATE(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
    /* Traverse descriptors owned by CPU */
    while ((desc_iter->RDES0.Own == EMAC_LL_DMADESC_OWNER_CPU) && (used_descs < CONFIG_ETH_DMA_RX_BUFFER_NUM)) {
        used_descs++;
        /* Last segment in frame */
        if (desc_iter->RDES0.LastDescriptor) {
            (*remain_frames)++;
        }
        /* point to next descriptor */
        desc_iter = (eth_dma_rx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
        DMA_CACHE_INVALIDATE(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
    }
    *free_descs = CONFIG_ETH_DMA_RX_BUFFER_NUM - used_descs;
}

uint8_t *emac_esp_dma_alloc_recv_buf(emac_esp_dma_handle_t emac_esp_dma, uint32_t *size)
{
    uint32_t ret_len = 0;
    uint32_t copy_len = 0;
    uint8_t *buf = NULL;

    if (emac_esp_dma_get_valid_recv_len(emac_esp_dma, &ret_len) != ESP_OK) {
        *size = 0;
        return NULL;
    }
    /* packets larger than expected will be truncated */
    copy_len = ret_len > *size ? *size : ret_len;

    if (copy_len > 0) {
        buf = malloc(copy_len);
        if (buf != NULL) {
            emac_esp_dma_auto_buf_info_t *buff_info = (emac_esp_dma_auto_buf_info_t *)buf;
            /* no need to check allocated buffer min length prior writing since we know that EMAC DMA is configured to
            not forward erroneous or undersized frames (less than 64B) on ESP32, see emac_hal_init_dma_default */
#ifndef NDEBUG
            buff_info->magic_id = EMAC_ALLOC_BUF_MAGIC_ID;
#endif // NDEBUG
            buff_info->copy_len = copy_len;
        }
    }
    /* indicate actual size of received frame */
    *size = ret_len;
    return buf;
}

uint32_t emac_esp_dma_receive_frame(emac_esp_dma_handle_t emac_esp_dma, uint8_t *buf, uint32_t size, eth_mac_time_t *ts)
{
    uint32_t ret_len = 0;
    uint32_t copy_len = 0;

    if (size != EMAC_DMA_BUF_SIZE_AUTO) {
        if (emac_esp_dma_get_valid_recv_len(emac_esp_dma, &ret_len) != ESP_OK) {
            return 0;
        }
        /* packets larger than expected will be truncated */
        copy_len = ret_len > size ? size : ret_len;
    } else {
        emac_esp_dma_auto_buf_info_t *buff_info = (emac_esp_dma_auto_buf_info_t *)buf;
#ifndef NDEBUG
        /* check that buffer was allocated by emac_esp_dma_alloc_recv_buf */
        assert(buff_info->magic_id == EMAC_ALLOC_BUF_MAGIC_ID);
#endif // NDEBUG
        copy_len = buff_info->copy_len;
        ret_len = copy_len;
    }

    if (copy_len) {
        eth_dma_rx_descriptor_t *desc_iter = emac_esp_dma->rx_desc;
        while (copy_len > CONFIG_ETH_DMA_BUFFER_SIZE) {
            DMA_CACHE_INVALIDATE(desc_iter->Buffer1Addr, CONFIG_ETH_DMA_BUFFER_SIZE);
            memcpy(buf, (void *)(desc_iter->Buffer1Addr), CONFIG_ETH_DMA_BUFFER_SIZE);
            buf += CONFIG_ETH_DMA_BUFFER_SIZE;
            copy_len -= CONFIG_ETH_DMA_BUFFER_SIZE;
            /* Set Own bit in Rx descriptors: gives the buffers back to DMA */
            desc_iter->RDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
            DMA_CACHE_WB(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
            desc_iter = (eth_dma_rx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
        }
        DMA_CACHE_INVALIDATE(desc_iter->Buffer1Addr, CONFIG_ETH_DMA_BUFFER_SIZE);
        memcpy(buf, (void *)(desc_iter->Buffer1Addr), copy_len);
        /* `copy_len` does not include CRC (which may be stored in separate buffer), hence check if we reached the last descriptor */
        while (!desc_iter->RDES0.LastDescriptor) {
            desc_iter->RDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
            DMA_CACHE_WB(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
            desc_iter = (eth_dma_rx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
        }
#if SOC_EMAC_IEEE1588V2_SUPPORTED
        if (ts != NULL) {
            if (emac_hal_get_rxdesc_timestamp(&emac_esp_dma->hal, desc_iter, &ts->seconds, &ts->nanoseconds) != ESP_OK) {
                /* zeros indicate invalid time stamp since it is not possible to ever get "zero time" under normal conditions */
                ts->seconds = 0;
                ts->nanoseconds = 0;
            }
        }
#endif
        /* return last descriptor to DMA */
        desc_iter->RDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
        DMA_CACHE_WB(desc_iter, EMAC_HAL_DMA_DESC_SIZE);

        /* update rxdesc */
        emac_esp_dma->rx_desc = (eth_dma_rx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
        /* poll rx demand */
        emac_hal_receive_poll_demand(&emac_esp_dma->hal);
    }
    return ret_len;
}

void emac_esp_dma_flush_recv_frame(emac_esp_dma_handle_t emac_esp_dma)
{
    eth_dma_rx_descriptor_t *desc_iter = emac_esp_dma->rx_desc;

    DMA_CACHE_INVALIDATE(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
    /* While not last descriptor => return back to DMA */
    while (!desc_iter->RDES0.LastDescriptor) {
        desc_iter->RDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
        DMA_CACHE_WB(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
        desc_iter = (eth_dma_rx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
    }
    /* the last descriptor */
    desc_iter->RDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
    DMA_CACHE_WB(desc_iter, EMAC_HAL_DMA_DESC_SIZE);
    /* update rxdesc */
    emac_esp_dma->rx_desc = (eth_dma_rx_descriptor_t *)(desc_iter->Buffer2NextDescAddr);
    /* poll rx demand */
    emac_hal_receive_poll_demand(&emac_esp_dma->hal);
}

esp_err_t emac_esp_del_dma(emac_esp_dma_handle_t emac_esp_dma)
{
    if (emac_esp_dma) {
        for (int i = 0; i < CONFIG_ETH_DMA_TX_BUFFER_NUM; i++) {
            free(emac_esp_dma->tx_buf[i]);
        }
        for (int i = 0; i < CONFIG_ETH_DMA_RX_BUFFER_NUM; i++) {
            free(emac_esp_dma->rx_buf[i]);
        }
        free(emac_esp_dma->descriptors);
        free(emac_esp_dma);
    }
    return ESP_OK;
}

esp_err_t emac_esp_new_dma(const emac_esp_dma_config_t *config, emac_esp_dma_handle_t *ret_handle)
{
    esp_err_t ret = ESP_OK;
    *ret_handle = NULL;
    struct emac_esp_dma_t *emac_esp_dma = calloc(1, sizeof(struct emac_esp_dma_t));
    ESP_GOTO_ON_FALSE(emac_esp_dma, ESP_ERR_NO_MEM, err, TAG, "no mem for esp emac_esp_dma object");

    /* alloc memory for ethernet dma descriptor */
    uint32_t desc_size = CONFIG_ETH_DMA_RX_BUFFER_NUM * sizeof(eth_dma_rx_descriptor_t) +
                         CONFIG_ETH_DMA_TX_BUFFER_NUM * sizeof(eth_dma_tx_descriptor_t);
    emac_esp_dma->descriptors = heap_caps_aligned_calloc(4, 1, desc_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(emac_esp_dma->descriptors, ESP_ERR_NO_MEM, err, TAG, "no mem for descriptors");
    /* alloc memory for ethernet dma buffer */
    for (int i = 0; i < CONFIG_ETH_DMA_RX_BUFFER_NUM; i++) {
        emac_esp_dma->rx_buf[i] = heap_caps_aligned_calloc(4, 1, CONFIG_ETH_DMA_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_GOTO_ON_FALSE(emac_esp_dma->rx_buf[i], ESP_ERR_NO_MEM, err, TAG, "no mem for RX DMA buffers");
    }
    for (int i = 0; i < CONFIG_ETH_DMA_TX_BUFFER_NUM; i++) {
        emac_esp_dma->tx_buf[i] = heap_caps_aligned_calloc(4, 1, CONFIG_ETH_DMA_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_GOTO_ON_FALSE(emac_esp_dma->tx_buf[i], ESP_ERR_NO_MEM, err, TAG, "no mem for TX DMA buffers");
    }
    emac_hal_init(&emac_esp_dma->hal);
    *ret_handle = emac_esp_dma;
    return ESP_OK;
err:
    emac_esp_del_dma(emac_esp_dma);
    return ret;
}
