#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"

#include "pb_encode.h"
#include "pb_decode.h"
#include "interface.pb.h"

#define USJ_TX_BUF_SIZE         8192
#define USJ_RX_BUF_SIZE         4096
#define FRAME_MAGIC_1           0xBE
#define FRAME_MAGIC_2           0xEF
#define MAX_PROTOBUF_SIZE       3000
#define SERIAL_TX_TIMEOUT_MS    100
#define SERIAL_RX_TIMEOUT_MS    50

#define SERIAL_RX_TASK_STACK    8192
#define SERIAL_RX_TASK_PRIORITY 2
#define PAYLOAD_QUEUE_LENGTH    5
#define MAX_PAYLOAD_SIZE        2304
#define SERIAL_RX_CHUNK_SIZE    512

typedef struct {
    uint8_t data[MAX_PAYLOAD_SIZE];
    size_t  len;
} payload_item_t;


typedef enum {
    RX_SYNC_1,
    RX_SYNC_2,
    RX_LENGTH,
    RX_DATA
} rx_state_t;

extern QueueHandle_t payload_queue;

void usj_init(void);
void serial_send_from_board(const its_FromBoard *msg);
void serial_rx_task(void *pvParameters);
