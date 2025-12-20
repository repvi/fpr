#pragma once

/**
 * @file fpr_handle.h
 * @brief FPR Protocol Version Handler
 * 
 * This module handles protocol version negotiation and compatibility
 * checking for incoming packets. It routes packets to the appropriate
 * handler based on their version:
 * - Legacy handler for pre-versioning (v0) packets
 * - Current handler for v1.x packets
 * - Future handler for newer protocol versions
 * 
 * @version 1.0.0
 * @date December 2024
 */

#include "fpr/fpr_lts.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle protocol version routing for received packets
 * 
 * Examines the version field in received packets and determines
 * which handler should process them:
 * - v0 (legacy): Routed to fpr_legacy_handle_protocol_v0()
 * - v1.x (current): Processed directly
 * - v2+ (future): Routed to fpr_new_handle_protocol_version()
 * 
 * @param esp_now_info ESP-NOW receive information
 * @param data Raw packet data
 * @param len Packet length
 * @param version Protocol version from packet header
 * @return true if packet should be processed, false if rejected
 */
bool fpr_version_handle_version(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len, code_version_t version);

#ifdef __cplusplus
}
#endif