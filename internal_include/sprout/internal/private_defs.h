#pragma once

/**
 * @file private_defs.h
 * @brief FPR Private Type Definitions and Structures
 * 
 * Internal data structures used by the FPR implementation.
 * These definitions are not part of the public API and may
 * change between versions.
 * 
 * @warning Internal API - subject to change without notice.
 * 
 * @version 1.0.0
 * @date December 2024
 */

#include "sprout/sprout_handle.h"
#include "sprout/sprout_def.h"
#include "sprout/sprout_config.h"
#include "sprout/sprout_security.h"
#include "version_control.h"
#include "base_macros.h"
#include "hashmap.h"
#include "hashmap_presets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_mac.h"
#include <string.h>

#define MAC_ADDRESS_LENGTH 6
#define FPR_CONNECT_NAME_SIZE 32

#define FPR_DEFAULT_MAX_HOPS 10

typedef struct {
    char name[FPR_CONNECT_NAME_SIZE];
    esp_now_peer_info_t peer_info;
    sprout_visibility_t visibility;
    uint8_t pwk[FPR_KEY_SIZE];  // Primary Working Key
    uint8_t lwk[FPR_KEY_SIZE];  // Local Working Key
    bool has_pwk;               // PWK is included
    bool has_lwk;               // LWK is included
} sprout_connect_t;

typedef enum {
    SPROUT_PACKAGE_TYPE_SINGLE = 0,
    SPROUT_PACKAGE_TYPE_START,
    SPROUT_PACKAGE_TYPE_CONTINUED,
    SPROUT_PACKAGE_TYPE_END
} sprout_package_type_t;

typedef struct {
    esp_now_peer_info_t peer_info;
    char name[PEER_NAME_MAX_LENGTH];
    QueueHandle_t response_queue;
    sprout_security_keys_t security;  // Security keys for this peer
    sprout_security_state_t sec_state; // Security handshake state
    bool is_connected;
    sprout_peer_state_t state;     // Connection state
    uint8_t hop_count;          // Distance from origin (0 = direct connection)
    uint8_t next_hop_mac[MAC_ADDRESS_LENGTH];    // MAC of next device in route (for forwarding)
    int64_t last_seen;          // Last time we heard from this peer (microseconds, esp_timer)
    int8_t rssi;                // Signal strength
    uint32_t packets_received;  // Packets received from this peer
    uint32_t last_seq_num;      // Last received sequence number (for replay protection)
    uint32_t queued_packets;    // Number of complete packets currently in queue
    sprout_queue_mode_t queue_mode; // Queue mode for this peer (defaults to global setting)
    bool receiving_fragmented;   // True if currently receiving a multi-fragment message
    uint32_t fragment_seq_num;   // Sequence number of the fragmented message being received
} sprout_store_hash_t;

#define FPR_STORE_HASH_TYPE sprout_store_hash_t
#define SPROUT_STORE_HASH_TYPE sprout_store_hash_t

#define FPR_BROADCAST_ADDRESS {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define FPR_QUEUE_LENGTH 20 // for enough data for incoming packets

#define FPR_PROTOCOL_DATA_INT_SIZE 45

typedef struct {
    union {
        int data_int[FPR_PROTOCOL_DATA_INT_SIZE];
        uint8_t general_data[FPR_PROTOCOL_DATA_INT_SIZE * sizeof(int)];
        sprout_connect_t connect_info;
        // customized struct here
    } protocol;
    
    sprout_package_type_t package_type;
    sprout_package_id_t id;
    
    // Routing fields for mesh forwarding
    uint8_t origin_mac[MAC_ADDRESS_LENGTH];      // Original sender
    uint8_t dest_mac[MAC_ADDRESS_LENGTH];        // Final destination (broadcast if all 0xFF)
    uint8_t hop_count;          // Current hop number
    uint8_t max_hops;           // Maximum allowed hops (TTL)
    code_version_t version;   // Protocol version
    
    uint16_t payload_size;      // Actual bytes used in protocol union for this packet
    uint32_t sequence_num;      // Sequence number for replay protection

    uint8_t reserved[10]; // Padding for alignment (reduced to account for sequence_num)
} sprout_package_t;

_Static_assert(ESP_NOW_MAX_DATA_LEN > sizeof(sprout_package_t), "ESP_NOW_MAX_DATA_LEN must be greater than sizeof(sprout_package_t)");

typedef struct {
    hashmap_t *peers_map;
    hashmap_t *allowlist_map;  // Whitelist of allowed MAC addresses
    bool allowlist_enabled;    // Whether allowlist filtering is active
    char name[PEER_NAME_MAX_LENGTH];
    uint8_t mac[MAC_ADDRESS_LENGTH];
    sprout_visibility_t access_state;
    esp_now_send_cb_t sender;
    esp_now_recv_cb_t receiver;
    sprout_mode_type_t current_mode;
    bool routing_enabled;       // Enable mesh routing/forwarding
    
    // Application data callback
    sprout_data_receive_cb_t data_callback;
    
    // Connection control (host mode)
    sprout_host_config_t host_config;
    
    // Connection control (client mode)
    sprout_client_config_t client_config;
    
    // Network statistics
    struct {
        uint32_t packets_sent;
        uint32_t packets_received;
        uint32_t packets_forwarded;
        uint32_t packets_dropped;
        uint32_t send_failures;
        uint32_t replay_attacks_blocked;  // Replay attack counter
    } stats;

    uint8_t host_pwk[FPR_KEY_SIZE];  // Host's Primary Working Key (host mode only)
    bool host_pwk_valid;              // Host PWK has been generated
    TaskHandle_t loop_task;
    TaskHandle_t reconnect_task;
    sprout_network_state_t state;        // Current network state
    bool paused;                      // Paused flag
    
    // Channel and power management
    uint8_t channel;                  // WiFi channel (1-14, 0 = auto)
    sprout_power_mode_t power_mode;      // Power management mode
    uint32_t tx_sequence_num;         // Outgoing sequence number counter
    
    // Queue management
    sprout_queue_mode_t default_queue_mode;  // Default queue mode for new peers
} sprout_network_t;


extern sprout_network_t sprout_net;