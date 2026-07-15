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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board_status.h"
#include "wifi_controller.h"
#include "serial_protocol.h"
#include "esp_check.h"

static const char *TAG = "C-ITS";

QueueHandle_t payload_queue;

#define HEARTBEAT_INTERVAL_MS   10000
#define HEARTBEAT_TASK_STACK    6144
#define HEARTBEAT_TASK_PRIORITY 1

static void heartbeat_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last_wake = xTaskGetTickCount();

    while (1)
    {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

        its_FromBoard msg = its_FromBoard_init_zero;
        msg.which_msg = its_FromBoard_heartbeat_tag;
        board_get_heartbeat_data(&msg.msg.heartbeat);

        CITS_LOGI(TAG, "[HB #%lu] uptime=%lus free_heap=%lu min_free=%lu "
                 "tx=%lld rx=%lld rx_dropped=%lu",
                 msg.msg.heartbeat.hb_seq,
                 msg.msg.heartbeat.uptime_s,
                 msg.msg.heartbeat.free_heap,
                 msg.msg.heartbeat.min_free_heap,
                 msg.msg.heartbeat.tx_count,
                 msg.msg.heartbeat.rx_count,
                 msg.msg.heartbeat.rx_dropped);

        CITS_LOGI(TAG, "Sending HB #%lu via serial", msg.msg.heartbeat.hb_seq);
        serial_send_from_board(&msg);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS needs erase, retrying...");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        ret = nvs_flash_init();
    }
    return ret;
}

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    ESP_LOGI(TAG, "ESP32-C5 802.11p ITS-G5 Transmitter");

    ESP_ERROR_CHECK(init_nvs());
    ESP_LOGI(TAG, "NVS initialised");

    /* preserves undocumented call order */
    ESP_ERROR_CHECK(wifi_controller_init());
    ESP_LOGI(TAG, "WiFi stack initialised, MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             wifi_controller_get_mac()[0], wifi_controller_get_mac()[1],
             wifi_controller_get_mac()[2], wifi_controller_get_mac()[3],
             wifi_controller_get_mac()[4], wifi_controller_get_mac()[5]);

    ESP_ERROR_CHECK(wifi_controller_phy_80211p_init());

    usj_init();
    ESP_LOGI(TAG, "USB Serial/JTAG initialised for protobuf serial protocol");

    payload_queue = xQueueCreate(PAYLOAD_QUEUE_LENGTH, sizeof(payload_item_t));
    assert(payload_queue);

    xTaskCreate(serial_rx_task, "serial_rx", SERIAL_RX_TASK_STACK,
                NULL, SERIAL_RX_TASK_PRIORITY, NULL);

    if (wifi_controller_start_promiscuous_rx() == ESP_OK)
    {
        wifi_controller_start_rx_print_task();
    }

    wifi_controller_start_tx_task();

    xTaskCreate(heartbeat_task, "heartbeat", HEARTBEAT_TASK_STACK,
                NULL, HEARTBEAT_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Heartbeat task started (interval=%dms)", HEARTBEAT_INTERVAL_MS);
}