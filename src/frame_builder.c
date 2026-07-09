/*
 * 802.11 QoS Data frame builder for ESP32-C5 ITS-G5 transmission.
 *
 * Builds a complete 802.11 QoS Data frame in a caller-provided buffer,
 * ready for submission via esp_wifi_80211_tx_custom().
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "frame_builder.h"

size_t build_qos_data_frame(uint8_t *buf, size_t buf_size,
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
