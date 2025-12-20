#pragma once

/**
 * @file fpr_extender.h
 * @brief FPR Extender/Mesh Mode Interface
 * 
 * @warning UNDER DEVELOPMENT - This module is not yet production-ready.
 *          Extender mode will be completed in version 1.1.0.
 * 
 * The extender mode enables mesh networking capabilities where devices
 * can relay packets to extend network range. When fully implemented,
 * it will provide:
 * - Multi-hop packet forwarding
 * - Automatic route discovery and maintenance
 * - TTL-based loop prevention
 * - Dynamic routing table updates
 * 
 * Current Status (v1.0.0):
 * - Basic packet forwarding structure is implemented
 * - Route table management is in place
 * - Full mesh networking requires additional testing
 * 
 * @version 1.0.0 (Under Development)
 * @date December 2024
 */

#include "fpr/internal/helpers.h"
#include "fpr/fpr_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle incoming packets in extender mode
 * 
 * @warning Internal function - do not call directly.
 * 
 * Processes received packets and determines whether to:
 * - Deliver locally (if addressed to this device)
 * - Forward to next hop (if addressed elsewhere)
 * - Both (for broadcast packets)
 * 
 * @param esp_now_info ESP-NOW receive information (source MAC, RSSI, etc.)
 * @param data Raw packet data
 * @param len Length of packet data
 */
void _handle_extender_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif

