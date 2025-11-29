#pragma once

#include <stdint.h>
#include "standard/time.h"

#define MAC_ADDRESS_LENGTH 6
#define PEER_NAME_MAX_LENGTH 32

typedef enum {
    FPR_VISIBILITY_PUBLIC = 0,
    FPR_VISIBILITY_PRIVATE
} fpr_visibility_t;

typedef int fpr_package_id_t;

typedef enum {
    FPR_MODE_DEFUALT = 0,
    FPR_MODE_CLIENT,
    FPR_MODE_HOST,
    FPR_MODE_BROADCAST,
    FPR_MODE_EXTENDER
} fpr_mode_type_t;

typedef enum {
    FPR_CONNECTION_AUTO = 0,    // Automatically accept/connect to discovered peers
    FPR_CONNECTION_MANUAL       // Require manual approval for connections
} fpr_connection_mode_t;

typedef enum fpr_peer_state_t {
    FPR_PEER_STATE_DISCOVERED = 0,  // Found but not connected
    FPR_PEER_STATE_PENDING,         // Connection request pending approval
    FPR_PEER_STATE_CONNECTED,       // Fully connected
    FPR_PEER_STATE_REJECTED,        // Connection rejected
    FPR_PEER_STATE_BLOCKED          // Blocked from connecting
} fpr_peer_state_t;

typedef void(*fpr_data_receive_cb_t)(void *peer_addr, void *data, void *user_data);

/**
 * @brief Callback for connection requests (host mode).
 * @param peer_mac MAC address of requesting peer.
 * @param peer_name Name of requesting peer.
 * @param peer_key Connection key from peer.
 * @return true to accept connection, false to reject.
 */
typedef bool(*fpr_connection_request_cb_t)(const uint8_t *peer_mac, const char *peer_name, uint32_t peer_key);

/**
 * @brief Callback for peer discovery (client mode).
 * @param peer_mac MAC address of discovered peer.
 * @param peer_name Name of discovered peer.
 * @param rssi Signal strength.
 */
typedef void(*fpr_peer_discovered_cb_t)(const uint8_t *peer_mac, const char *peer_name, int8_t rssi);

typedef struct {
    char name[PEER_NAME_MAX_LENGTH];
    uint8_t mac[MAC_ADDRESS_LENGTH];
    bool is_connected;
    fpr_peer_state_t state;
    uint8_t hop_count;
    int8_t rssi;
    uint64_t last_seen_ms;
    uint32_t packets_received;
} fpr_peer_info_t;

typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_forwarded;
    uint32_t packets_dropped;
    uint32_t send_failures;
    size_t peer_count;
} fpr_network_stats_t;

typedef struct {
    fpr_package_id_t package_id;
    uint8_t max_hops;
} fpr_send_options_t;

typedef struct {
    uint8_t max_peers;                          // Maximum peers allowed (0 = unlimited)
    fpr_connection_mode_t connection_mode;      // Auto or manual connection approval
    fpr_connection_request_cb_t request_cb;     // Callback for manual approval (NULL for auto)
} fpr_host_config_t;

typedef struct {
    fpr_connection_mode_t connection_mode;      // Auto or manual connection
    fpr_peer_discovered_cb_t discovery_cb;      // Callback when peers are discovered
} fpr_client_config_t;