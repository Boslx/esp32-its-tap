/* Frame format: 0xBE 0xEF [uint32 LE length] [protobuf bytes] */

#include "serial_protocol.h"
#include "board_status.h"

#include "driver/usb_serial_jtag.h"

static const char *SERIAL_TAG = "SERIAL";

static SemaphoreHandle_t s_serial_tx_mutex;

void usj_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USJ_TX_BUF_SIZE,
        .rx_buffer_size = USJ_RX_BUF_SIZE,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));

    s_serial_tx_mutex = xSemaphoreCreateMutex();
    assert(s_serial_tx_mutex);
}

void serial_send_from_board(const its_FromBoard *msg)
{
    if (xSemaphoreTake(s_serial_tx_mutex, pdMS_TO_TICKS(SERIAL_TX_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(SERIAL_TAG, "serial TX mutex timeout");
        return;
    }

    uint8_t *pb_buf = (uint8_t *)malloc(MAX_PROTOBUF_SIZE);
    if (!pb_buf)
    {
        ESP_LOGE(SERIAL_TAG, "pb_buf alloc failed");
        xSemaphoreGive(s_serial_tx_mutex);
        return;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, MAX_PROTOBUF_SIZE);
    if (!pb_encode(&stream, its_FromBoard_fields, msg))
    {
        ESP_LOGE(SERIAL_TAG, "pb_encode failed: %s", PB_GET_ERROR(&stream));
        free(pb_buf);
        xSemaphoreGive(s_serial_tx_mutex);
        return;
    }

    uint32_t pb_len = (uint32_t)stream.bytes_written;

    size_t frame_len = 2 + 4 + pb_len;
    uint8_t *frame = (uint8_t *)malloc(frame_len);
    if (!frame)
    {
        free(pb_buf);
        ESP_LOGE(SERIAL_TAG, "frame alloc failed");
        xSemaphoreGive(s_serial_tx_mutex);
        return;
    }

    size_t off = 0;
    frame[off++] = FRAME_MAGIC_1;
    frame[off++] = FRAME_MAGIC_2;
    frame[off++] = (uint8_t)(pb_len & 0xFF);
    frame[off++] = (uint8_t)((pb_len >> 8) & 0xFF);
    frame[off++] = (uint8_t)((pb_len >> 16) & 0xFF);
    frame[off++] = (uint8_t)((pb_len >> 24) & 0xFF);
    memcpy(frame + off, pb_buf, pb_len);

    usb_serial_jtag_write_bytes(frame, off + pb_len, pdMS_TO_TICKS(SERIAL_TX_TIMEOUT_MS));

    free(pb_buf);
    free(frame);
    xSemaphoreGive(s_serial_tx_mutex);
}

static bool handle_to_board_message(const its_ToBoard *msg)
{
    switch (msg->which_msg)
    {
    case its_ToBoard_request_hb_tag:
    {
        its_FromBoard reply = its_FromBoard_init_zero;
        reply.which_msg = its_FromBoard_heartbeat_tag;
        board_get_heartbeat_data(&reply.msg.heartbeat);
        serial_send_from_board(&reply);
        return true;
    }

    case its_ToBoard_frame_tag:
    {
        size_t fsize = msg->msg.frame.frame_data.size;
        if (fsize == 0 || fsize > MAX_PAYLOAD_SIZE)
        {
            ESP_LOGW(SERIAL_TAG, "Invalid frame size: %u", fsize);
            return false;
        }

        payload_item_t item;
        item.len = fsize;
        memcpy(item.data, msg->msg.frame.frame_data.bytes, fsize);

        if (xQueueSend(payload_queue, &item, 0) != pdTRUE)
        {
            ESP_LOGW(SERIAL_TAG, "Frame queue full, dropping PC message");
            return false;
        }

        CITS_LOGI(SERIAL_TAG, "Got %u-byte frame from PC, queued for TX", item.len);
        return true;
    }

    default:
        ESP_LOGW(SERIAL_TAG, "Unknown ToBoard oneof type %d", msg->which_msg);
        return false;
    }
}

static bool try_decode_to_board(const uint8_t *rx_pb, uint32_t pb_len)
{
    its_ToBoard msg = its_ToBoard_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(rx_pb, pb_len);

    if (!pb_decode(&istream, its_ToBoard_fields, &msg))
    {
        ESP_LOGE(SERIAL_TAG, "pb_decode ToBoard failed: %s", PB_GET_ERROR(&istream));
        return false;
    }

    return handle_to_board_message(&msg);
}

static bool rx_state_machine_byte(uint8_t byte,
                                  rx_state_t *state,
                                  uint32_t *expected_len,
                                  uint32_t *bytes_read,
                                  uint8_t *rx_pb)
{
    switch (*state)
    {
    case RX_SYNC_1:
        if (byte == FRAME_MAGIC_1)
            *state = RX_SYNC_2;
        break;

    case RX_SYNC_2:
        if (byte == FRAME_MAGIC_2)
        {
            *state = RX_LENGTH;
            *expected_len = 0;
            *bytes_read = 0;
        }
        else
        {
            *state = RX_SYNC_1;
        }
        break;

    case RX_LENGTH:
        ((uint8_t *)expected_len)[*bytes_read] = byte;
        (*bytes_read)++;
        if (*bytes_read >= 4)
        {
            if (*expected_len == 0 || *expected_len > MAX_PROTOBUF_SIZE)
            {
                ESP_LOGW(SERIAL_TAG, "Invalid protobuf length: %lu", *expected_len);
                *state = RX_SYNC_1;
            }
            else
            {
                *bytes_read = 0;
                *state = RX_DATA;
            }
        }
        break;

    case RX_DATA:
        rx_pb[(*bytes_read)++] = byte;
        if (*bytes_read >= *expected_len)
        {
            try_decode_to_board(rx_pb, *expected_len);
            *state = RX_SYNC_1;
            return true;
        }
        break;
    }
    return false;
}

void serial_rx_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t buf[SERIAL_RX_CHUNK_SIZE];
    rx_state_t state = RX_SYNC_1;
    uint32_t expected_len = 0;
    uint32_t bytes_read = 0;
    static uint8_t rx_pb[MAX_PROTOBUF_SIZE];

    while (1)
    {
        int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(SERIAL_RX_TIMEOUT_MS));
        if (len <= 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (int i = 0; i < len; i++)
        {
            rx_state_machine_byte(buf[i], &state, &expected_len, &bytes_read, rx_pb);
        }
    }
}
