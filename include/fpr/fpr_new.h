#pragma once

/**
 * @file fpr_new.h
 * @brief Future Protocol Version Handlers
 * 
 * This module handles packets from newer FPR protocol versions that
 * this device doesn't fully understand. It provides forward compatibility
 * by attempting to extract compatible fields from future packet formats.
 * 
 * When This Handler is Used:
 * - A device running v1.0.0 receives a packet from a v2.x device
 * - The packet format may have changed, but core fields may be compatible
 * - This handler attempts graceful degradation rather than dropping packets
 * 
 * Compatibility Strategy:
 * - Extract basic packet fields (MAC addresses, hop count, etc.)
 * - Process what can be understood, ignore unknown fields
 * - Log warnings about version mismatch for debugging
 * 
 * @version 1.0.0
 * @date December 2025
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
 * @brief Handle packets from future protocol versions
 * 
 * Attempts to process packets from newer protocol versions that may
 * have extended or modified packet formats. If the packet structure
 * is compatible enough, it extracts what it can.
 * 
 * @param version Protocol version of the received packet
 * @param esp_now_info ESP-NOW receive info with source MAC, RSSI, etc.
 * @param data Raw packet data
 * @param len Packet length
 * @return true if packet was processed successfully (even partially)
 * @return false if packet should be dropped (incompatible)
 */
bool fpr_new_handle_protocol_version(code_version_t version, const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif
