#pragma once

#include <stdint.h>
#include <stddef.h>
#include "interface.pb.h"

/* Firmware version string included in heartbeat messages */
#define FIRMWARE_VERSION "1.0.0"

/* Set to ESP_LOGI to re-enable heartbeat/TX/RX frame logs */
#ifndef CITS_LOGI
#define CITS_LOGI(...) do {} while(0)
#endif

void board_status_init(const uint8_t own_mac[6]);
void board_status_count_tx(void);
void board_status_count_rx(void);
void board_status_count_rx_dropped(void);
uint32_t board_status_consume_rx_dropped(void);
int64_t board_status_get_tx_count(void);
void board_get_heartbeat_data(its_Heartbeat *hb);
