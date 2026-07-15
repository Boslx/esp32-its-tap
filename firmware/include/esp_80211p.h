/*
 * esp_80211p.h Undocumented ESP32-C5 802.11p structures and function declarations
 *
 * This file contains reverse-engineered data structures and function prototypes
 * for the undocumented 802.11p TX path on ESP32-C5. These are extracted from
 * Espressif's closed-source binary libraries (libphy.a, libcore.a, libnet80211.a).
 *
 * WARNING: These structures are ABI-sensitive and tied to the exact library
 * versions from esp-wifi-sys-esp32c5 v0.2.0 (git SHA 1139b322).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Internal WiFi buffer structures - reverse-engineered from binary libs.
 * Must use default C alignment (no #pragma pack) to match #[repr(C)] layout. */

struct x_ebuf_t;
struct x_eb_txdesc_t;

/* Linked-list data segment node (0x10 = 16 bytes) */
struct x_middle_data_t
{
    uint32_t field_40;
    uint8_t *buf;
    uint32_t field_48;
    uint32_t field_4c;
};

/* TX descriptor (0x48 = 72 bytes) - controls frame transmission */
struct x_eb_txdesc_t
{
    uint32_t flags;     /* 0x00 - bit 14=custom TX, bit 0=sys_seq */
    uint32_t field_4;   /* 0x04 */
    uint32_t field_8;   /* 0x08 - bit 15 = BW (0=20MHz, 1=40MHz) */
    uint8_t rate;       /* 0x0C */
    uint8_t field_d;    /* 0x0D */
    uint8_t field_e;    /* 0x0E */
    uint8_t field_f;    /* 0x0F */
    uint32_t field_10;  /* 0x10 - bits 18-19 = interface index */
    uint32_t field_14;  /* 0x14 */
    uint32_t timestamp; /* 0x18 */
    void *sched;        /* 0x1C - from ic_get_default_sched() */
    uint32_t field_20;  /* 0x20 */
    uint32_t field_24;  /* 0x24 */
    uint32_t field_28;  /* 0x28 */
    uint32_t field_2c;  /* 0x2C */
    uint32_t field_30;  /* 0x30 */
    uint32_t field_34;  /* 0x34 */
    uint32_t field_38;  /* 0x38 */
    uint32_t field_3c;  /* 0x3C */
    uint32_t field_40;  /* 0x40 */
    uint32_t field_44;  /* 0x44 */
};

/* Internal WiFi packet buffer (0x40 = 64 bytes) with data segment chain.
 *
 * WARNING: Implicit padding:
 *   2 bytes after header_length (0x16-0x17)
 *   3 bytes after field_2c (0x2D-0x2F)
 * DO NOT use #pragma pack! */
struct x_ebuf_t
{
    uint32_t field_0;                /* 0x00 */
    struct x_middle_data_t *ds_head; /* 0x04 */
    struct x_middle_data_t *ds_tail; /* 0x08 */
    uint16_t field_c;                /* 0x0C */
    uint16_t field_e;                /* 0x0E */
    uint32_t extra_data_start;       /* 0x10 */
    uint16_t header_length;          /* 0x14 */
    /* 2 bytes padding */
    uint32_t data_length;            /* 0x18 */
    uint16_t field_1c;               /* 0x1C */
    uint8_t alloc_type;              /* 0x1E */
    uint8_t field_1f;                /* 0x1F */
    uint32_t field_20;               /* 0x20 */
    uint8_t field_24;                /* 0x24 */
    uint8_t field_25;                /* 0x25 */
    uint8_t field_26;                /* 0x26 */
    uint8_t field_27;                /* 0x27 */
    uint32_t field_28;               /* 0x28 */
    uint8_t field_2c;                /* 0x2C */
    /* 3 bytes padding */
    uint32_t field_30;               /* 0x30 */
    uint32_t next_free;              /* 0x34 */
    struct x_eb_txdesc_t *txdesc;    /* 0x38 */
    uint16_t field_3c;               /* 0x3C */
    uint8_t field_3e;                /* 0x3E */
    uint8_t field_3f;                /* 0x3F */
};

/* Compile-time size assertions */
_Static_assert(sizeof(struct x_ebuf_t) == 0x40, "x_ebuf_t must be 64 bytes");
_Static_assert(sizeof(struct x_eb_txdesc_t) == 0x48, "x_eb_txdesc_t must be 72 bytes");
_Static_assert(sizeof(struct x_middle_data_t) == 0x10, "x_middle_data_t must be 16 bytes");

/* Undocumented library functions */
extern void phy_11p_set(int enable, int reserved);
extern void phy_change_channel(int channel, int unknown1, int unknown2, int ht_mode);
extern void phy_disable_cca(void);
extern struct x_ebuf_t *ic_ebuf_alloc(const void *packet, uint32_t unknown, uint32_t len);
extern void *ic_get_default_sched(void);
extern esp_err_t ieee80211_post_hmac_tx(void *ebuf);

/* Global symbols from libcore.a */
extern void *g_osi_funcs_p;
extern void *g_wifi_global_lock;

/* PHY rate configuration */
struct wifi_tx_rate_config_t
{
    unsigned int phymode;
    unsigned int rate;
    bool ersu;
    bool dcm;
};

/** Send an 802.11 frame via the custom (undocumented) TX path.
 * @param ifx             Interface index (WIFI_IF_STA = 0)
 * @param buffer          Complete 802.11 frame (header + payload)
 * @param len             Frame length
 * @param en_sys_seq      Enable system-assigned sequence number
 * @param tx_rate_config  PHY rate configuration
 * @param band            WiFi band (2 = 5 GHz)
 * @param bw              Bandwidth (1 = 20 MHz, 2 = 40 MHz)
 * @return ESP_OK or ESP_ERR_NO_MEM */
int esp_wifi_80211_tx_custom(
    unsigned int ifx,
    const void *buffer,
    int len,
    bool en_sys_seq,
    struct wifi_tx_rate_config_t *tx_rate_config,
    unsigned int band,
    unsigned int bw);

#define MAC_ADDR_LEN 6
