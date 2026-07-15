/* CONSTRAINT: Undocumented WiFi calls and their order are NOT changed. */

#include "wifi_controller.h"
#include "board_status.h"
#include "serial_protocol.h"
#include "modem_clocks.h"
#include "esp_80211p.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_private/wifi_os_adapter.h"
#include "soc/soc.h"

static const char *TAG = "C-ITS";

static uint8_t s_own_mac[6] = {0};

#define RX_QUEUE_LENGTH         10
/* small -- stack-allocated in ISR callback */
#define RX_FRAME_DATA_BYTES     512
#define RX_PRINT_TASK_STACK     8192
#define RX_PRINT_TASK_PRIORITY  3

#define FRAME_BUF_SIZE          2304
#define NUM_FRAME_BUFS          2
#define SEND_TASK_STACK         4096
#define SEND_TASK_PRIORITY      5

typedef struct
{
    uint8_t src_mac[6];
    int16_t rssi;
    uint16_t len;
    uint8_t data[RX_FRAME_DATA_BYTES];
} rx_frame_info_t;

static QueueHandle_t s_rx_queue;

static void send_task(void *pvParameters);
static void rx_print_task(void *pvParameters);
static void its_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type);

esp_err_t wifi_controller_init(void)
{
    modem_clocks_init(); /* before esp_wifi_init, clocks PHY I2C bus */

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    cfg.sta_disconnected_pm = false;
    cfg.tx_hetb_queue_num = 3;
    cfg.dump_hesigb_enable = false;
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init failed");

    wifi_country_t country = {
        .cc = "DE",
        .schan = 1,
        .nchan = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_country(&country), TAG, "set_country failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps failed");

    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocols(WIFI_IF_STA, &protocols), TAG, "set_protocols failed");

    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, s_own_mac), TAG, "get_mac failed");
    board_status_init(s_own_mac);

    return ESP_OK;
}

const uint8_t *wifi_controller_get_mac(void)
{
    return s_own_mac;
}

esp_err_t wifi_controller_phy_80211p_init(void)
{
    ESP_LOGI(TAG, "Enabling 802.11p PHY mode...");
    phy_11p_set(PHY_11P_ENABLED, PHY_11P_RESERVED); /* undocumented */

    esp_err_t ch_ret = esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE); /* 5 GHz for calibration */
    if (ch_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "set_channel(140) returned %d", ch_ret);
    }

    ESP_LOGI(TAG, "Forcing radio to 5900 MHz (ITS-G5 Ch180)...");
    phy_change_channel(PHY_CHANNEL_5900, PHY_CH_UNKNOWN_1, PHY_CH_UNKNOWN_2, PHY_CH_HT_MODE_20);
    ESP_LOGI(TAG, "802.11p ready on 5900 MHz");
    return ESP_OK;
}

esp_err_t wifi_controller_start_promiscuous_rx(void)
{
    s_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(rx_frame_info_t));
    if (s_rx_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create RX queue");
        return ESP_ERR_NO_MEM;
    }

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_filter(&filter), TAG, "set_promiscuous_filter failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(its_rx_cb), TAG, "set_promiscuous_rx_cb failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous(true), TAG, "set_promiscuous failed");
    ESP_LOGI(TAG, "Promiscuous RX enabled on 5900 MHz");
    return ESP_OK;
}

void wifi_controller_start_tx_task(void)
{
    xTaskCreate(send_task, "send_80211p", SEND_TASK_STACK,
                NULL, SEND_TASK_PRIORITY, NULL);
}

void wifi_controller_start_rx_print_task(void)
{
    xTaskCreate(rx_print_task, "rx_print", RX_PRINT_TASK_STACK,
                NULL, RX_PRINT_TASK_PRIORITY, NULL);
}

static void send_task(void *pvParameters)
{
    (void)pvParameters;
    static uint8_t frame_buf[NUM_FRAME_BUFS][FRAME_BUF_SIZE];
    int buf_idx = 0;
    uint16_t seq = 0;

    struct wifi_tx_rate_config_t tx_config = {
        .phymode = WIFI_PHY_MODE_11A,
        .rate    = WIFI_PHY_RATE_12M,
        .ersu    = false,
        .dcm     = false,
    };

    while (1)
    {
        payload_item_t item;
        if (xQueueReceive(payload_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (item.len < MIN_FRAME_LEN)
        {
            ESP_LOGW(TAG, "Frame too short (%u bytes), need at least %d",
                     item.len, MIN_FRAME_LEN);
            continue;
        }

        uint8_t *buf = frame_buf[buf_idx];
        buf_idx = 1 - buf_idx;

        memcpy(buf, item.data, item.len);

        const uint16_t seq_ctrl = (seq & 0x0FFF) << 4; /* Sequence Control at bytes 22-23 */
        buf[22] = seq_ctrl & 0xFF;
        buf[23] = (seq_ctrl >> 8) & 0xFF;

        int result = esp_wifi_80211_tx_custom( /* undocumented */
            0, buf, item.len, true, &tx_config,
            WIFI_BAND_5G,
            WIFI_BW20
        );

        if (result == ESP_OK)
        {
            board_status_count_tx();
            CITS_LOGI(TAG, "TX #%lld: %u bytes, seq=%u", board_status_get_tx_count(), item.len, seq);
        }
        else
        {
            ESP_LOGE(TAG, "TX failed: %d", result);
        }

        seq = (seq + 1) % 4096;
    }
}

static void its_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint16_t frame_len = pkt->rx_ctrl.sig_len;

    if (frame_len < 24) /* too small for valid 802.11 header */
    {
        return;
    }

    rx_frame_info_t info;
    memset(&info, 0, sizeof(info));

    memcpy(info.src_mac, pkt->payload + 10, 6); /* Address 2 = source MAC */
    info.rssi = pkt->rx_ctrl.rssi;
    info.len = frame_len;

    uint16_t copy_len = (frame_len < RX_FRAME_DATA_BYTES) ? frame_len : RX_FRAME_DATA_BYTES;
    memcpy(info.data, pkt->payload, copy_len);

    BaseType_t higher_woken = pdFALSE;
    if (xQueueSendFromISR(s_rx_queue, &info, &higher_woken) != pdTRUE)
    {
        board_status_count_rx_dropped();
    }

    if (higher_woken)
    {
        portYIELD_FROM_ISR();
    }
}

static void rx_print_task(void *pvParameters)
{
    (void)pvParameters;
    rx_frame_info_t info;

    while (1)
    {
        if (xQueueReceive(s_rx_queue, &info, portMAX_DELAY) == pdTRUE)
        {
            board_status_count_rx();

            CITS_LOGI(TAG, "RX: len=%d rssi=%d src=%02X:%02X:%02X:%02X:%02X:%02X",
                     info.len, info.rssi,
                     info.src_mac[0], info.src_mac[1], info.src_mac[2],
                     info.src_mac[3], info.src_mac[4], info.src_mac[5]);

            its_ReceivedFrame rf = its_ReceivedFrame_init_zero;
            rf.timestamp_us = (uint64_t)esp_timer_get_time();
            memcpy(rf.src_mac.bytes, info.src_mac, 6);
            rf.src_mac.size = 6;
            rf.rssi = info.rssi;

            uint16_t copy_len = (info.len < RX_FRAME_DATA_BYTES) ? info.len : RX_FRAME_DATA_BYTES;
            memcpy(rf.frame.frame_data.bytes, info.data, copy_len);
            rf.frame.frame_data.size = copy_len;
            rf.has_frame = true;

            its_FromBoard from_msg = its_FromBoard_init_zero;
            from_msg.which_msg = its_FromBoard_received_frame_tag;
            from_msg.msg.received_frame = rf;

            serial_send_from_board(&from_msg);

            uint32_t dropped = board_status_consume_rx_dropped();
            if (dropped > 0)
            {
                ESP_LOGW(TAG, "Dropped %lu RX frames (queue full)", dropped);
            }
        }
    }
}
