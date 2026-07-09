/*
 * Custom 802.11 frame transmission for ESP32-C5.
 *
 * Bypasses the normal ESP-IDF WiFi stack to directly allocate internal
 * buffers, configure TX descriptors, and submit frames for transmission.
 * Uses the IDF's wifi_osi_funcs_t for safe function pointer access.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/wifi_os_adapter.h"

#include "esp_80211p.h"

static const char *TAG = "C-ITS-TX";

int esp_wifi_80211_tx_custom(
    unsigned int ifx,
    const void *buffer,
    int len,
    bool en_sys_seq,
    struct wifi_tx_rate_config_t *tx_rate_config,
    unsigned int band,
    unsigned int bw)
{
    wifi_osi_funcs_t *osi = (wifi_osi_funcs_t *)g_osi_funcs_p;
    if (osi == NULL)
    {
        ESP_LOGE(TAG, "g_osi_funcs_p is NULL");
        return ESP_FAIL;
    }

    if (osi->_mutex_lock)
    {
        osi->_mutex_lock(g_wifi_global_lock);
    }
    else
    {
        ESP_LOGW(TAG, "_mutex_lock is NULL, skipping lock");
    }

    struct x_ebuf_t *eb = ic_ebuf_alloc(buffer, 1, len);
    if (eb == NULL)
    {
        ESP_LOGE(TAG, "ic_ebuf_alloc failed");
        if (osi->_mutex_unlock)
        {
            osi->_mutex_unlock(g_wifi_global_lock);
        }
        return ESP_ERR_NO_MEM;
    }

    struct x_eb_txdesc_t *txdesc = eb->txdesc;

    eb->data_length = 0;
    eb->header_length = (uint16_t)len;
    txdesc->flags |= 0x4000; /* custom TX */
    txdesc->sched = ic_get_default_sched();

    /* Set PHY rate */
    unsigned int rate = tx_rate_config->rate;
    if (rate != 0)
    {
        txdesc->rate = (uint8_t)rate;
    }
    else if (band != 2)
    { /* WIFI_BAND_5G */
        txdesc->rate = 0;
    }
    else
    {
        txdesc->rate = 11; /* WIFI_PHY_RATE_6M */
    }

    /* Configure PHY mode */
    unsigned int phymode = tx_rate_config->phymode;
    if (phymode == 6)
    { /* WIFI_PHY_MODE_HE20 */
        txdesc->flags |= 0x80000000;
        uint32_t v = txdesc->field_2c;
        v = ((((tx_rate_config->ersu ? 1U : 0U) + 6) & 0x0F) << 3) | (v & 0x87);
        txdesc->field_2c = v;
        if (tx_rate_config->dcm)
        {
            ((uint8_t *)&txdesc->field_30)[1] |= 0x80;
        }
    }
    else if (phymode == 7)
    { /* WIFI_PHY_MODE_VHT20 */
        txdesc->flags |= 0x01000000;
    }

    uint32_t bw_is_bw40 = (bw == 2) ? 1 : 0; /* WIFI_BW40 = 2 */
    txdesc->field_8 = (bw_is_bw40 << 15) | (txdesc->field_8 & 0xFFFF7FFF);

    if (en_sys_seq)
    {
        txdesc->flags |= 1;
    }

    txdesc->field_10 = (txdesc->field_10 & 0xFFF3FFFF) | ((ifx & 3) << 18);
    txdesc->field_14 = 0x100;

    esp_err_t ret = ieee80211_post_hmac_tx(eb);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "ieee80211_post_hmac_tx failed: %d", ret);
    }

    if (osi->_mutex_unlock)
    {
        osi->_mutex_unlock(g_wifi_global_lock);
    }

    return ret;
}

/* printf stub called by proprietary .a libraries */
void __esp_radio_printf(const char *tag, const char *msg)
{
    if (tag && msg)
    {
        ESP_LOGI("PHY", "%s %s", tag, msg);
    }
}
