#pragma once

/**
 * @file fpr_host.h
 * @brief FPR Host Mode Interface
 * 
 * Host mode allows a device to act as a central coordinator that accepts
 * connections from client devices. The host manages:
 * - Client discovery and connection acceptance
 * - Security handshake (PWK/LWK key exchange)
 * - Connection limits and peer management
 * - Auto/manual connection approval modes
 * 
 * Features:
 * - Automatic client discovery via broadcast
 * - WiFi WPA2-style 4-way security handshake
 * - Configurable maximum peer limits
 * - Manual connection approval with callback
 * - Peer blocking and unblocking
 * - Keepalive and reconnection support
 * 
 * @version 1.0.0 (Stable)
 * @date December 2024
 */

#include "fpr/internal/helpers.h"
#include "fpr/fpr_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle incoming packets in host mode
 * 
 * @warning Internal function - do not call directly.
 * 
 * Processes received packets from clients including:
 * - Discovery requests (new client wants to connect)
 * - Security handshake responses (PWK/LWK exchange)
 * - Application data from connected clients
 * 
 * @param esp_now_info ESP-NOW receive information (source MAC, RSSI, etc.)
 * @param data Raw packet data
 * @param len Length of packet data
 */
void _handle_host_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

/**
 * @brief Background task for host reconnection/keepalive
 * 
 * @warning Internal function - started via fpr_network_start_reconnect_task().
 * 
 * Monitors connection state and sends periodic keepalives to maintain
 * connections with clients.
 * 
 * @param arg Task arguments (unused)
 */
void _fpr_host_reconnect_task(void *arg);

#ifdef __cplusplus
}
#endif