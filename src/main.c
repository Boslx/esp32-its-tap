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
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_private/wifi_os_adapter.h"
#include "nvs_flash.h"
#include "soc/soc.h"

#include "esp_80211p.h"
#include "modem_clocks.h"
#include "frame_builder.h"

#include "esp_system.h"
#include "esp_heap_caps.h"

static const char *TAG = "C-ITS";

#define TX_INTERVAL_MS 1000
#define FRAME_BUF_SIZE 256
#define NUM_FRAME_BUFS 2
#define SEND_TASK_STACK 4096
#define SEND_TASK_PRIORITY 5

/* ---- RX (Promiscuous Mode) ---- */
#define RX_QUEUE_LENGTH 10
#define RX_PRINT_DATA_BYTES 64
#define RX_PRINT_TASK_STACK 3072
#define RX_PRINT_TASK_PRIORITY 3

/* Small struct to transfer frame info from ISR callback to print task */
typedef struct
{
    uint8_t src_mac[6];
    int16_t rssi;
    uint16_t len;
    uint8_t data[RX_PRINT_DATA_BYTES];
} rx_frame_info_t;

static QueueHandle_t rx_queue;
static uint32_t dropped_rx_frames = 0;
static int64_t tx_count = 0;
static int64_t rx_count = 0;

/* Heartbeat task */
#define HEARTBEAT_INTERVAL_MS 10000
#define HEARTBEAT_TASK_STACK 2048
#define HEARTBEAT_TASK_PRIORITY 1

static void heartbeat_task(void *pvParameters);

static void rx_print_task(void *pvParameters);
static void its_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type);

static void send_task(void *pvParameters);

static void wifi_init(void)
{
    /* Must happen before esp_wifi_init() so PHY I2C bus is clocked */
    modem_clocks_init();

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
            // ESP_LOGI(TAG, "TX #%d: %d bytes, seq=%u", tx_count, frame_len, seq);
        }
        else
        {
            ESP_LOGE(TAG, "TX failed: %d", result);
        }

        seq = (seq + 1) % 4096;
        vTaskDelay(pdMS_TO_TICKS(TX_INTERVAL_MS));
    }
}

/* Heartbeat task: prints debug info every 10 seconds with a sequence number */
static void heartbeat_task(void *pvParameters)
{
    uint32_t hb_seq = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1)
    {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_free_heap = esp_get_minimum_free_heap_size();
        int64_t uptime_ms = esp_timer_get_time() / 1000;
        uint32_t uptime_s = (uint32_t)(uptime_ms / 1000);

        ESP_LOGI(TAG, "[HB #%lu] uptime=%lus free_heap=%lu min_free=%lu tx=%lld rx=%lld rx_dropped=%lu",
                 hb_seq, uptime_s, free_heap, min_free_heap, tx_count, rx_count, dropped_rx_frames);

        hb_seq++;
    }
}

/* Promiscuous mode RX callback — runs in ISR context.
 * Copies frame info into a queue for safe printing in rx_print_task. */
static void its_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint16_t frame_len = pkt->rx_ctrl.sig_len;

    /* Skip frames that are too small to have a valid 802.11 header (24 bytes min) */
    if (frame_len < 24)
    {
        return;
    }

    rx_frame_info_t info;
    memset(&info, 0, sizeof(info));

    /* Source MAC is at offset 10 (Address 2) in the 802.11 header */
    memcpy(info.src_mac, pkt->payload + 10, 6);
    info.rssi = pkt->rx_ctrl.rssi;
    info.len = frame_len;

    /* Copy up to RX_PRINT_DATA_BYTES of the frame for hex dump */
    uint16_t copy_len = (frame_len < RX_PRINT_DATA_BYTES) ? frame_len : RX_PRINT_DATA_BYTES;
    memcpy(info.data, pkt->payload, copy_len);

    /* ISR-safe: non-blocking send to queue */
    BaseType_t higher_woken = pdFALSE;
    if (xQueueSendFromISR(rx_queue, &info, &higher_woken) != pdTRUE)
    {
        dropped_rx_frames++;
    }

    if (higher_woken)
    {
        portYIELD_FROM_ISR();
    }
}

/* Task: dequeue received frame info and print it */
static void rx_print_task(void *pvParameters)
{
    (void)pvParameters;
    rx_frame_info_t info;

    while (1)
    {
        if (xQueueReceive(rx_queue, &info, portMAX_DELAY) == pdTRUE)
        {
            rx_count++;

            ESP_LOGI(TAG, "RX: len=%d rssi=%d src=%02X:%02X:%02X:%02X:%02X:%02X",
                     info.len, info.rssi,
                     info.src_mac[0], info.src_mac[1], info.src_mac[2],
                     info.src_mac[3], info.src_mac[4], info.src_mac[5]);

            ESP_LOG_BUFFER_HEX(TAG, info.data,
                               (info.len < RX_PRINT_DATA_BYTES) ? info.len : RX_PRINT_DATA_BYTES);

            if (dropped_rx_frames > 0)
            {
                ESP_LOGW(TAG, "Dropped %lu RX frames (queue full)", dropped_rx_frames);
                dropped_rx_frames = 0;
            }
        }
    }
}

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    ESP_LOGI(TAG, "ESP32-C5 802.11p ITS-G5 Transmitter");

    /* ---- Initialize NVS (required by ESP-IDF PHY calibration storage) ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase, retrying...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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

    /* ---- RX setup: promiscuous mode on 5900 MHz ---- */
    rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(rx_frame_info_t));
    if (rx_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create RX queue");
    }
    else
    {
        wifi_promiscuous_filter_t filter = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT};
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(its_rx_cb));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
        ESP_LOGI(TAG, "Promiscuous RX enabled on 5900 MHz");

        xTaskCreate(rx_print_task, "rx_print", RX_PRINT_TASK_STACK,
                    NULL, RX_PRINT_TASK_PRIORITY, NULL);
    }

    xTaskCreate(send_task, "send_80211p", SEND_TASK_STACK,
                own_mac, SEND_TASK_PRIORITY, NULL);

    xTaskCreate(heartbeat_task, "heartbeat", HEARTBEAT_TASK_STACK,
                NULL, HEARTBEAT_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Heartbeat task started (interval=%dms)", HEARTBEAT_INTERVAL_MS);
}