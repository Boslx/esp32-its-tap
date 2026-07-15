#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

#define PHY_11P_ENABLED     1
#define PHY_11P_RESERVED    0
#define PHY_CHANNEL_5900    5900
#define PHY_CH_UNKNOWN_1    1
#define PHY_CH_UNKNOWN_2    0
#define PHY_CH_HT_MODE_20   0
#define WIFI_BAND_5G        2
#define WIFI_BW20           1
#define WIFI_PHY_MODE_11A   3
#define WIFI_PHY_RATE_12M   10
#define MIN_FRAME_LEN       26

esp_err_t wifi_controller_init(void);
esp_err_t wifi_controller_phy_80211p_init(void);
esp_err_t wifi_controller_start_promiscuous_rx(void);
void wifi_controller_start_tx_task(void);
void wifi_controller_start_rx_print_task(void);
const uint8_t *wifi_controller_get_mac(void);
