#pragma once

/**
 * @file sprout_def.h
 * @brief FPR Type Definitions and Data Structures
 * 
 * This header defines all public types, enums, and structures used
 * throughout the FPR library. These definitions provide the foundation
 * for network configuration, peer management, and data transmission.
 * 
 * @version 1.0.0
 * @date December 2025
 */

#include <stdint.h>
#include <time.h>

#define MAC_ADDRESS_LENGTH 6
#define PEER_NAME_MAX_LENGTH 32

typedef enum {
    SPROUT_VISIBILITY_PUBLIC = 0,
    SPROUT_VISIBILITY_PRIVATE
} sprout_visibility_t;

/**
 * @brief Power management modes for FPR network.
 * 
 * LOW_POWER mode increases polling intervals to save battery.
 * NORMAL mode uses default intervals for responsive communication.
 */
typedef enum {
    SPROUT_POWER_NORMAL = 0,       // Normal power mode (default intervals)
    SPROUT_POWER_LOW               // Low power mode (longer intervals, less responsive)
} sprout_power_mode_t;

/**
 * @brief Queue modes for incoming data packets.
 * 
 * Controls how the receive queue handles incoming packets when the consumer
 * cannot keep up with the producer (sender).
 * 
 * NORMAL: Queue all packets in order. Consumer processes them sequentially.
 *         May cause delays if processing is slow.
 * 
 * LATEST_ONLY: Keep only the most recent complete packet, discard older ones.
 *              Useful for real-time status updates where only the latest data
 *              matters (e.g., sensor readings, position updates).
 *              Eliminates processing delay at the cost of discarding old data.
 */
typedef enum {
    SPROUT_QUEUE_MODE_NORMAL = 0,   // Queue all packets (default)
    SPROUT_QUEUE_MODE_LATEST_ONLY   // Keep only the latest complete packet
} sprout_queue_mode_t;

typedef enum {
    SPROUT_STATE_UNINITIALIZED = 0,  // Not initialized
    SPROUT_STATE_INITIALIZED,        // Initialized but not started
    SPROUT_STATE_STARTED,            // Started and running
    SPROUT_STATE_PAUSED,             // Paused (can be resumed)
    SPROUT_STATE_STOPPED             // Stopped (can be restarted)
} sprout_network_state_t;

typedef int sprout_package_id_t;

/**
 * @brief Reserved packet ID for connection/handshake control packets.
 * 
 * This ID is used internally for:
 * - Device discovery broadcasts
 * - Connection requests
 * - Security handshake (PWK/LWK exchange)
 * - Reconnection requests
 * 
 * Application data should use any other ID (0 or positive values recommended).
 */
#define SPROUT_PACKET_ID_CONTROL (-1)

typedef enum {
    SPROUT_MODE_DEFAULT = 0,
    SPROUT_MODE_CLIENT,
    SPROUT_MODE_HOST,
    SPROUT_MODE_BROADCAST,
    SPROUT_MODE_EXTENDER
} sprout_mode_type_t;

typedef enum {
    SPROUT_CONNECTION_AUTO = 0,    // Automatically accept/connect to discovered peers
    SPROUT_CONNECTION_MANUAL       // Require manual approval for connections
} sprout_connection_mode_t;

typedef enum sprout_peer_state_t {
    SPROUT_PEER_STATE_DISCOVERED = 0,  // Found but not connected
    SPROUT_PEER_STATE_PENDING,         // Connection request pending approval
    SPROUT_PEER_STATE_CONNECTED,       // Fully connected
    SPROUT_PEER_STATE_REJECTED,        // Connection rejected
    SPROUT_PEER_STATE_BLOCKED          // Blocked from connecting
} sprout_peer_state_t;

typedef void(*sprout_data_receive_cb_t)(void *peer_addr, void *data, void *user_data);

/**
 * @brief Callback for connection requests (host mode).
 * @param peer_mac MAC address of requesting peer.
 * @param peer_name Name of requesting peer.
 * @param peer_key Connection key from peer.
 * @return true to accept connection, false to reject.
 */
typedef bool(*sprout_connection_request_cb_t)(const uint8_t *peer_mac, const char *peer_name, uint32_t peer_key);

/**
 * @brief Callback for peer discovery (client mode).
 * @param peer_mac MAC address of discovered peer.
 * @param peer_name Name of discovered peer.
 * @param rssi Signal strength.
 */
typedef void(*sprout_peer_discovered_cb_t)(const uint8_t *peer_mac, const char *peer_name, int8_t rssi);

/**
 * @brief Callback for host selection (client mode, manual connection).
 * Called when a host is discovered and manual connection mode is active.
 * Application should decide whether to connect to this host.
 * @param peer_mac MAC address of discovered host.
 * @param peer_name Name of discovered host.
 * @param rssi Signal strength.
 * @return true to connect to this host, false to skip.
 */
typedef bool(*sprout_host_selection_cb_t)(const uint8_t *peer_mac, const char *peer_name, int8_t rssi);

typedef struct {
    char name[PEER_NAME_MAX_LENGTH];
    uint8_t mac[MAC_ADDRESS_LENGTH];
    bool is_connected;
    sprout_peer_state_t state;
    int8_t rssi;
    uint8_t hop_count;
    uint64_t last_seen_ms;
    uint32_t packets_received;
} sprout_peer_info_t;

typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_dropped;
    uint32_t send_failures;
    size_t peer_count;
} sprout_network_stats_t;

/**
 * @brief Host mode configuration
 */
typedef struct {
    int max_peers;                             // Maximum number of connected clients (0 = unlimited)
    sprout_connection_mode_t connection_mode;  // Auto or manual connection approval
    sprout_connection_request_cb_t request_cb; // Callback for connection requests (manual mode)
} sprout_host_config_t;

/**
 * @brief Client mode configuration
 */
typedef struct {
    sprout_connection_mode_t connection_mode;  // Auto or manual connection
    sprout_peer_discovered_cb_t discovery_cb; // Callback when hosts are discovered
    sprout_host_selection_cb_t selection_cb;  // Callback for manual host selection
} sprout_client_config_t;
