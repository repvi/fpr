#pragma once

/**
 * @file fpr_new.h
 * @brief Future protocol version handlers for forward compatibility
 * 
 * This header defines handlers for newer FPR protocol versions that
 * may be received from devices running future firmware.
 * 
 * When a device receives packets from a newer protocol version, it can
 * attempt to gracefully handle them or at least process compatible fields.
 * 
 * Version History:
 * - v1.0.0: Current version
 * - Future versions will be added here as protocol evolves
 */

#include "fpr/fpr_config.h"
#include "fpr/fpr_lts.h"
#include "esp_now.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle future protocol version packets
 * 
 * This handler attempts to process packets from newer protocol versions.
 * If the packet structure is compatible enough, it extracts what it can.
 * 
 * @param version Protocol version of the received packet
 * @param esp_now_info ESP-NOW receive info with source MAC, RSSI, etc.
 * @param data Raw packet data
 * @param len Packet length
 * @return true if packet was processed successfully
 * @return false if packet should be dropped
 */
bool fpr_new_handle_protocol_version(code_version_t version, const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif
