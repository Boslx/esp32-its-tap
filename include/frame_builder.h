#pragma once

#include <stdint.h>
#include <stddef.h>

/** Build an 802.11 QoS Data frame (26-byte header + payload, FCS added by HW).
 *  Returns total frame length, or 0 if buffer too small.
 *
 *  Frame layout:
 *    [0-1]   Frame Control:      0x8800  (QoS Data, no flags)
 *    [2-3]   Duration/ID:        0x0000
 *    [4-9]   Address 1 (DA):     FF:FF:FF:FF:FF:FF (broadcast)
 *    [10-15] Address 2 (SA):     Own MAC address
 *    [16-21] Address 3 (BSSID):  FF:FF:FF:FF:FF:FF (wildcard = OCB)
 *    [22-23] Sequence Control:   (seq_num << 4) | 0
 *    [24-25] QoS Control:        0x20 0x00 (TID=0, Ack=Normal)
 *    [26+]   Payload:            Application data
 */
size_t build_qos_data_frame(uint8_t *buf, size_t buf_size,
                            const uint8_t own_mac[6],
                            uint16_t seq,
                            const uint8_t *payload, size_t payload_len);
