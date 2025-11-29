#pragma once

/**
 * @file fpr_legacy.h
 * @brief Legacy protocol handlers for backward compatibility
 * 
 * This header defines handlers for older FPR protocol versions.
 * When the protocol changes in a breaking way, add a new handler here
 * to convert old packet formats to the current format.
 * 
 * Version History:
 * - v0 (legacy): Pre-versioning era, no version field set
 * - v1.0.0: Initial versioned protocol with fragmentation support
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
 * @brief Handle legacy v0 protocol packets (pre-versioning era)
 * 
 * Legacy v0 packets had no version field populated (version=0).
 * The protocol union layout was the same as v1, just version wasn't set.
 * 
 * @param esp_now_info ESP-NOW receive info with source MAC, RSSI, etc.
 * @param data Raw packet data
 * @param len Packet length
 * @return true if packet can be processed with current handler
 * @return false if packet should be dropped
 */
bool fpr_legacy_handle_protocol_v0(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

/**
 * @brief Check if a packet appears to be legacy format
 * 
 * @param data Raw packet data
 * @param len Packet length
 * @return true if packet appears to be legacy format
 */
bool fpr_legacy_is_v0_packet(const uint8_t *data, int len);

bool fpr_legacy_handle_protocol_version(code_version_t version, const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif