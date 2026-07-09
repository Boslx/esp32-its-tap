/* ESP32-C5 IEEE 802.11p (ITS-G5) Frame Transmitter
 *
 * Transmits ITS-G5 frames on channel 180 (5900 MHz) using undocumented
 * Espressif PHY functions and the reverse-engineered internal WiFi TX path.
 * Uses ESP-IDF 5.5.4 built-in WiFi/PHY libraries which already contain
 * the undocumented 802.11p symbols (phy_11p_set, phy_change_channel).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_private/wifi_os_adapter.h"

#include "esp_80211p.h"

static const char *TAG = "C-ITS";

#define TX_INTERVAL_MS 100
#define FRAME_BUF_SIZE 256
#define NUM_FRAME_BUFS 2
#define SEND_TASK_STACK 4096
#define SEND_TASK_PRIORITY 5

static void send_task(void *pvParameters);

/* Builds an 802.11 QoS Data frame (26-byte header + payload, FCS added by HW):
 *   [0-1]   Frame Control:      0x8800  (QoS Data, no flags)
 *   [2-3]   Duration/ID:        0x0000
 *   [4-9]   Address 1 (DA):     FF:FF:FF:FF:FF:FF (broadcast)
 *   [10-15] Address 2 (SA):     Own MAC address
 *   [16-21] Address 3 (BSSID):  FF:FF:FF:FF:FF:FF (wildcard = OCB)
 *   [22-23] Sequence Control:   (seq_num << 4) | 0
 *   [24-25] QoS Control:        0x20 0x00 (TID=0, Ack=Normal)
 *   [26+]   Payload:            Application data
 */
static size_t build_qos_data_frame(uint8_t *buf, size_t buf_size,
                                   const uint8_t own_mac[6],
                                   uint16_t seq,
                                   const uint8_t *payload, size_t payload_len)
{
    const size_t header_len = 26;
    const size_t total_len = header_len + payload_len;

    if (buf_size < total_len)
    {
        return 0;
    }

    size_t off = 0;

    /* Frame Control: version(0) | type(Data=10) | subtype(QoSData=001000) = 0x88, flags = 0x00 */
    buf[off++] = 0x88;
    buf[off++] = 0x00;

    /* Duration / ID */
    buf[off++] = 0x00;
    buf[off++] = 0x00;

    /* Address 1 - Destination: broadcast */
    memset(buf + off, 0xFF, 6);
    off += 6;

    /* Address 2 - Source: own MAC */
    memcpy(buf + off, own_mac, 6);
    off += 6;

    /* Address 3 - BSSID: wildcard (OCB mode) */
    memset(buf + off, 0xFF, 6);
    off += 6;

    /* Sequence Control: fragment_number(4) | sequence_number(12), shifted left by 4 */
    const uint16_t seq_ctrl = (seq & 0x0FFF) << 4;
    buf[off++] = seq_ctrl & 0xFF;
    buf[off++] = (seq_ctrl >> 8) & 0xFF;

    /* QoS Control: TID=0, EOSP=0, Ack Policy=Normal */
    buf[off++] = 0x20;
    buf[off++] = 0x00;

    /* Payload */
    if (payload && payload_len > 0)
    {
        memcpy(buf + off, payload, payload_len);
        off += payload_len;
    }

    return off;
}

/* Bypasses the normal ESP-IDF WiFi stack to directly allocate internal
 * buffers, configure TX descriptors, and submit frames for transmission.
 * Uses the IDF's wifi_osi_funcs_t for safe function pointer access.
 */
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

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    cfg.sta_disconnected_pm = false;
    cfg.tx_hetb_queue_num = 3;
    cfg.dump_hesigb_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_country_t country = {
        .cc = "DE",
        .schan = 1,
        .nchan = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    ESP_ERROR_CHECK(esp_wifi_set_protocols(WIFI_IF_STA, &protocols));
}

static void send_task(void *pvParameters)
{
    const uint8_t *own_mac = (const uint8_t *)pvParameters;
    uint8_t frame_buf[NUM_FRAME_BUFS][FRAME_BUF_SIZE];
    int buf_idx = 0;
    uint16_t seq = 0;
    int tx_count = 0;

    const uint8_t dummy_payload[] = {0xC0, 0x01, 0xD0, 0x0D};

    struct wifi_tx_rate_config_t tx_config = {
        .phymode = 3, /* WIFI_PHY_MODE_11A */
        .rate = 10,   /* WIFI_PHY_RATE_12M */
        .ersu = false,
        .dcm = false,
    };

    /* Build and log first frame for verification */
    size_t first_len = build_qos_data_frame(frame_buf[0], FRAME_BUF_SIZE,
                                            own_mac, 0,
                                            dummy_payload, sizeof(dummy_payload));
    ESP_LOGI(TAG, "First frame (%d bytes):", first_len);
    ESP_LOG_BUFFER_HEX(TAG, frame_buf[0], first_len);

    while (1)
    {
        uint8_t *buf = frame_buf[buf_idx];
        buf_idx = 1 - buf_idx;

        size_t frame_len = build_qos_data_frame(buf, FRAME_BUF_SIZE,
                                                own_mac, seq,
                                                dummy_payload, sizeof(dummy_payload));
        if (frame_len == 0)
        {
            ESP_LOGE(TAG, "Frame buffer too small!");
            vTaskDelay(pdMS_TO_TICKS(TX_INTERVAL_MS));
            continue;
        }

        int result = esp_wifi_80211_tx_custom(
            0, buf, frame_len, true, &tx_config,
            2, /* WIFI_BAND_5G */
            1  /* WIFI_BW20 */
        );

        if (result == ESP_OK)
        {
            tx_count++;
            ESP_LOGI(TAG, "TX #%d: %d bytes, seq=%u", tx_count, frame_len, seq);
        }
        else
        {
            ESP_LOGE(TAG, "TX failed: %d", result);
        }

        seq = (seq + 1) % 4096;
        vTaskDelay(pdMS_TO_TICKS(TX_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    ESP_LOGI(TAG, "ESP32-C5 802.11p ITS-G5 Transmitter");

    wifi_init();

    uint8_t own_mac[MAC_ADDR_LEN];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, own_mac));
    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             own_mac[0], own_mac[1], own_mac[2],
             own_mac[3], own_mac[4], own_mac[5]);

    ESP_LOGI(TAG, "Enabling 802.11p PHY mode...");
    phy_11p_set(1, 0);

    /* Use a regular 5 GHz channel first for PHY calibration, then force 5900 MHz */
    esp_err_t ch_ret = esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE);
    if (ch_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "set_channel(140) returned %d", ch_ret);
    }

    ESP_LOGI(TAG, "Forcing radio to 5900 MHz (ITS-G5 Ch180)...");
    phy_change_channel(5900, 1, 0, 0);

    ESP_LOGI(TAG, "802.11p ready on 5900 MHz");

    xTaskCreate(send_task, "send_80211p", SEND_TASK_STACK,
                own_mac, SEND_TASK_PRIORITY, NULL);
}