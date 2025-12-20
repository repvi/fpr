#pragma once

/**
 * @file fpr_client.h
 * @brief FPR Client Mode Interface
 * 
 * Client mode allows a device to discover and connect to host devices.
 * The client handles:
 * - Host discovery via broadcast scanning
 * - Security handshake initiation (PWK/LWK key exchange)
 * - Connection state management
 * - Auto/manual host selection modes
 * 
 * Features:
 * - Automatic host discovery
 * - WiFi WPA2-style 4-way security handshake
 * - Host scanning with RSSI reporting
 * - Manual host selection with callback
 * - Automatic reconnection on disconnect
 * - Keepalive monitoring
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
 * @brief Handle host discovery packets in client mode
 * 
 * @warning Internal function - do not call directly.
 * 
 * Processes received packets from hosts including:
 * - Broadcast discovery announcements from hosts
 * - Security handshake packets (PWK from host)
 * - Connection acknowledgments
 * - Application data from connected host
 * 
 * @param esp_now_info ESP-NOW receive information (source MAC, RSSI, etc.)
 * @param data Raw packet data
 * @param len Length of packet data
 */
void _handle_client_discovery(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

/**
 * @brief Callback helper for finding hosts during scanning
 * 
 * @warning Internal function - used by hashmap iteration.
 * 
 * @param key Hashmap key (MAC address)
 * @param value Peer data structure
 * @param user_data User-provided context
 */
void _find_host_callback(void *key, void *value, void *user_data);

/**
 * @brief Background task for client reconnection/keepalive
 * 
 * @warning Internal function - started via fpr_network_start_reconnect_task().
 * 
 * Monitors connection state and attempts reconnection if disconnected.
 * Also sends periodic keepalives to maintain connection.
 * 
 * @param arg Task arguments (unused)
 */
void _fpr_client_reconnect_task(void *arg);

#ifdef __cplusplus
}
#endif