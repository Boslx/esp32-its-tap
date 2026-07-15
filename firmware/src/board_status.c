#include "board_status.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <string.h>

static int64_t  s_tx_count        = 0;
static int64_t  s_rx_count        = 0;
static uint32_t s_rx_dropped      = 0;
static uint8_t  s_own_mac[6]      = {0};
static uint32_t s_hb_seq          = 0;

void board_status_init(const uint8_t own_mac[6])
{
    s_tx_count   = 0;
    s_rx_count   = 0;
    s_rx_dropped = 0;
    s_hb_seq     = 0;
    memcpy(s_own_mac, own_mac, 6);
}

void board_status_count_tx(void)
{
    s_tx_count++;
}

void board_status_count_rx(void)
{
    s_rx_count++;
}

void board_status_count_rx_dropped(void)
{
    s_rx_dropped++;
}

int64_t board_status_get_tx_count(void)
{
    return s_tx_count;
}

uint32_t board_status_consume_rx_dropped(void)
{
    uint32_t val = s_rx_dropped;
    s_rx_dropped = 0;
    return val;
}

void board_get_heartbeat_data(its_Heartbeat *hb)
{
    hb->hb_seq        = s_hb_seq++;
    hb->uptime_s      = (uint32_t)(esp_timer_get_time() / 1000000);
    hb->free_heap     = esp_get_free_heap_size();
    hb->min_free_heap = esp_get_minimum_free_heap_size();
    hb->tx_count      = s_tx_count;
    hb->rx_count      = s_rx_count;
    hb->rx_dropped    = s_rx_dropped;
    memcpy(hb->src_mac.bytes, s_own_mac, 6);
    hb->src_mac.size = 6;
}
