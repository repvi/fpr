#pragma once

#include "fpr/fpr_handle.h"
#include "fpr/fpr_def.h"
#include "fpr/fpr_config.h"
#include "fpr/fpr_security.h"
#include "lib/version_control.h"
#include "lib/hashmap.h"
#include "lib/hashmap_presets.h"
#include "lib/base_macros.h"
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
    fpr_visibility_t visibility;
    uint8_t pwk[FPR_KEY_SIZE];  // Primary Working Key
    uint8_t lwk[FPR_KEY_SIZE];  // Local Working Key
    bool has_pwk;               // PWK is included
    bool has_lwk;               // LWK is included
} fpr_connect_t;

typedef enum {
    FPR_PACKAGE_TYPE_SINGLE = 0,
    FPR_PACKAGE_TYPE_START,
    FPR_PACKAGE_TYPE_CONTINUED,
    FPR_PACKAGE_TYPE_END
} fpr_package_type_t;

typedef struct {
    esp_now_peer_info_t peer_info;
    char name[PEER_NAME_MAX_LENGTH];
    QueueHandle_t response_queue;
    fpr_security_keys_t security;  // Security keys for this peer
    fpr_security_state_t sec_state; // Security handshake state
    bool is_connected;
    fpr_peer_state_t state;     // Connection state
    uint8_t hop_count;          // Distance from origin (0 = direct connection)
    uint8_t next_hop_mac[MAC_ADDRESS_LENGTH];    // MAC of next device in route (for forwarding)
    int64_t last_seen;          // Last time we heard from this peer (microseconds, esp_timer)
    int8_t rssi;                // Signal strength
    uint32_t packets_received;  // Packets received from this peer
} fpr_store_hash_t;

#define FPR_STORE_HASH_TYPE fpr_store_hash_t

#define FPR_BROADCAST_ADDRESS {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define FPR_QUEUE_LENGTH 10

#define FPR_PROTOCOL_DATA_INT_SIZE 45

typedef struct {
    union {
        int data_int[FPR_PROTOCOL_DATA_INT_SIZE];
        uint8_t general_data[FPR_PROTOCOL_DATA_INT_SIZE * sizeof(int)];
        fpr_connect_t connect_info;
        // customized struct here
    } protocol;
    
    fpr_package_type_t package_type;
    fpr_package_id_t id;
    
    // Routing fields for mesh forwarding
    uint8_t origin_mac[MAC_ADDRESS_LENGTH];      // Original sender
    uint8_t dest_mac[MAC_ADDRESS_LENGTH];        // Final destination (broadcast if all 0xFF)
    uint8_t hop_count;          // Current hop number
    uint8_t max_hops;           // Maximum allowed hops (TTL)
    code_version_t version;   // Protocol version

    uint8_t reserved[16]; // Padding for alignment
} fpr_package_t;

_Static_assert(ESP_NOW_MAX_DATA_LEN > sizeof(fpr_package_t), "ESP_NOW_MAX_DATA_LEN must be greater than sizeof(fpr_package_t)");

typedef struct {
    HashMap peers_map;
    char name[PEER_NAME_MAX_LENGTH];
    uint8_t mac[MAC_ADDRESS_LENGTH];
    fpr_visibility_t access_state;
    esp_now_send_cb_t sender;
    esp_now_recv_cb_t receiver;
    fpr_mode_type_t current_mode;
    bool routing_enabled;       // Enable mesh routing/forwarding
    
    // Application data callback
    fpr_data_receive_cb_t data_callback;
    
    // Connection control (host mode)
    fpr_host_config_t host_config;
    
    // Connection control (client mode)
    fpr_client_config_t client_config;
    
    // Network statistics
    struct {
        uint32_t packets_sent;
        uint32_t packets_received;
        uint32_t packets_forwarded;
        uint32_t packets_dropped;
        uint32_t send_failures;
    } stats;

    uint8_t host_pwk[FPR_KEY_SIZE];  // Host's Primary Working Key (host mode only)
    bool host_pwk_valid;              // Host PWK has been generated
    TaskHandle_t loop_task;
    TaskHandle_t reconnect_task;
    fpr_network_state_t state;        // Current network state
    bool paused;                      // Paused flag
} fpr_network_t;


extern fpr_network_t fpr_net;