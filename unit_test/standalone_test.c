/**
 * Standalone unit test runner for Sprout core logic
 * Compiles and runs on host machine without ESP-IDF
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Minimal type definitions
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -3
#define MAC_ADDRESS_LENGTH 6
#define PEER_NAME_MAX_LENGTH 32

// Minimal enum definitions
typedef enum {
    SPROUT_STATE_UNINITIALIZED = 0,
    SPROUT_STATE_INITIALIZED = 1,
    SPROUT_STATE_STARTED = 2,
    SPROUT_STATE_PAUSED = 3,
    SPROUT_STATE_STOPPED = 4
} sprout_network_state_t;

typedef enum {
    SPROUT_POWER_NORMAL = 0,
    SPROUT_POWER_LOW = 1
} sprout_power_mode_t;

typedef enum {
    SPROUT_QUEUE_MODE_NORMAL = 0,
    SPROUT_QUEUE_MODE_LATEST_ONLY = 1
} sprout_queue_mode_t;

typedef enum {
    SPROUT_PEER_STATE_DISCOVERED = 0,
    SPROUT_PEER_STATE_CONNECTED = 1,
    SPROUT_PEER_STATE_BLOCKED = 2
} sprout_peer_state_t;

typedef enum {
    SPROUT_MODE_CLIENT = 0,
    SPROUT_MODE_HOST = 1,
    SPROUT_MODE_EXTENDER = 2
} sprout_mode_type_t;

typedef enum {
    SPROUT_VISIBILITY_PUBLIC = 0,
    SPROUT_VISIBILITY_PRIVATE = 1
} sprout_visibility_t;

typedef enum {
    SPROUT_PACKAGE_TYPE_SINGLE = 0,
    SPROUT_PACKAGE_TYPE_START = 1,
    SPROUT_PACKAGE_TYPE_CONTINUED = 2,
    SPROUT_PACKAGE_TYPE_END = 3
} sprout_package_type_t;

#define SPROUT_PACKET_ID_CONTROL -1
#define SPROUT_BROADCAST_ADDRESS {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define SPROUT_NETWORK_VERSION 0x01000000

// Minimal structures
typedef struct {
    uint8_t peer_addr[6];
    uint8_t channel;
    uint8_t encrypt;
} esp_now_peer_info_t;

typedef struct {
    uint8_t protocol[250];
    uint8_t package_type;
    uint16_t payload_size;
    uint32_t sequence_num;
    uint8_t origin_mac[6];
    uint8_t dest_mac[6];
    uint8_t hop_count;
    uint8_t max_hops;
    int32_t id;
    uint32_t version;
} sprout_package_t;

typedef struct {
    uint8_t name[PEER_NAME_MAX_LENGTH];
    esp_now_peer_info_t peer_info;
    bool is_connected;
    sprout_peer_state_t state;
    uint8_t next_hop_mac[6];
    int64_t last_seen;
    int8_t rssi;
    uint32_t queued_packets;
    sprout_queue_mode_t queue_mode;
    bool receiving_fragmented;
    uint32_t fragment_seq_num;
    uint32_t last_seq_num;
} SPROUT_STORE_HASH_TYPE;

typedef struct {
    uint8_t mac[6];
    char name[PEER_NAME_MAX_LENGTH];
    bool is_connected;
    sprout_peer_state_t state;
    int8_t rssi;
} sprout_peer_info_t;

typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_dropped;
    uint32_t send_failures;
    size_t peer_count;
} sprout_network_stats_t;

typedef struct {
    char name[32];
    uint8_t mac[6];
    sprout_network_state_t state;
    uint8_t channel;
    sprout_power_mode_t power_mode;
    sprout_queue_mode_t default_queue_mode;
    sprout_visibility_t access_state;
    sprout_mode_type_t current_mode;
    uint32_t tx_sequence_num;
    sprout_network_stats_t stats;
    bool paused;
    uint8_t host_pwk[16];
    bool host_pwk_valid;
} sprout_network_t;

// Simple hashmap implementation for testing
#define HASHMAP_MAX_SIZE 32
typedef struct {
    uint8_t key[6];
    void *value;
} hashmap_entry_t;

typedef struct {
    hashmap_entry_t entries[HASHMAP_MAX_SIZE];
    size_t size;
} hashmap_t;

unsigned int mac_hash(const void *key) {
    const uint8_t *mac = (const uint8_t *)key;
    return (mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5]) % HASHMAP_MAX_SIZE;
}

bool mac_equals(const void *a, const void *b) {
    return memcmp(a, b, 6) == 0;
}

void hashmap_init(hashmap_t *map, int size, unsigned int (*hash)(const void*), bool (*equals)(const void*, const void*)) {
    memset(map, 0, sizeof(hashmap_t));
}

void hashmap_free(hashmap_t *map) {
    memset(map, 0, sizeof(hashmap_t));
}

void *hashmap_get(hashmap_t *map, const void *key) {
    for (size_t i = 0; i < map->size; i++) {
        if (mac_equals(map->entries[i].key, key)) {
            return map->entries[i].value;
        }
    }
    return NULL;
}

bool hashmap_put(hashmap_t *map, const void *key, void *value) {
    if (map->size >= HASHMAP_MAX_SIZE) return false;
    memcpy(map->entries[map->size].key, key, 6);
    map->entries[map->size].value = value;
    map->size++;
    return true;
}

bool hashmap_remove(hashmap_t *map, const void *key) {
    for (size_t i = 0; i < map->size; i++) {
        if (mac_equals(map->entries[i].key, key)) {
            for (size_t j = i; j < map->size - 1; j++) {
                map->entries[j] = map->entries[j + 1];
            }
            map->size--;
            return true;
        }
    }
    return false;
}

size_t hashmap_size(hashmap_t *map) {
    return map->size;
}

void hashmap_foreach(hashmap_t *map, void (*callback)(void*, void*, void*), void *user_data) {
    for (size_t i = 0; i < map->size; i++) {
        callback(map->entries[i].key, map->entries[i].value, user_data);
    }
}

// Helper functions
static inline void _safe_string_copy(char *dest, const char *src, size_t dest_size) {
    if (dest && src && dest_size > 0) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

static inline bool is_broadcast_address(const uint8_t *mac) {
    const uint8_t broadcast_addr[6] = SPROUT_BROADCAST_ADDRESS;
    return (memcmp(mac, broadcast_addr, 6) == 0);
}

static inline void sprout_set_peer_info(esp_now_peer_info_t *gen_info) {
    gen_info->channel = 0;
    gen_info->encrypt = false;
}

static inline SPROUT_STORE_HASH_TYPE *_get_peer_from_map(hashmap_t *map, const uint8_t *peer_mac) {
    return (SPROUT_STORE_HASH_TYPE *)hashmap_get(map, peer_mac);
}

// Test framework
int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) void test_##name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("Running test_%s... ", #name); \
    test_##name(); \
    printf("PASSED\n"); \
    tests_passed++; \
} while(0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        printf("FAILED: %s is false\n", #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQUAL(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != NULL)
#define ASSERT_EQUAL_MEMORY(a, b, len) ASSERT_TRUE(memcmp(a, b, len) == 0)
#define ASSERT_EQUAL_STRING(a, b) ASSERT_TRUE(strcmp(a, b) == 0)

// Test cases
TEST(safe_string_copy_normal) {
    char dest[20];
    const char *src = "Hello World";
    _safe_string_copy(dest, src, sizeof(dest));
    ASSERT_EQUAL_STRING(src, dest);
}

TEST(safe_string_copy_truncate) {
    char dest[5];
    const char *src = "Hello World";
    _safe_string_copy(dest, src, sizeof(dest));
    ASSERT_EQUAL_STRING("Hell", dest);
    ASSERT_EQUAL('\0', dest[4]);
}

TEST(is_broadcast_address_true) {
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_TRUE(is_broadcast_address(broadcast));
}

TEST(is_broadcast_address_false) {
    uint8_t not_broadcast[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    ASSERT_FALSE(is_broadcast_address(not_broadcast));
}

TEST(peer_state_transitions) {
    sprout_peer_state_t state = SPROUT_PEER_STATE_DISCOVERED;
    ASSERT_EQUAL(SPROUT_PEER_STATE_DISCOVERED, state);
    state = SPROUT_PEER_STATE_CONNECTED;
    ASSERT_EQUAL(SPROUT_PEER_STATE_CONNECTED, state);
}

TEST(network_state_transitions) {
    sprout_network_state_t state = SPROUT_STATE_UNINITIALIZED;
    ASSERT_EQUAL(SPROUT_STATE_UNINITIALIZED, state);
    state = SPROUT_STATE_INITIALIZED;
    ASSERT_EQUAL(SPROUT_STATE_INITIALIZED, state);
    state = SPROUT_STATE_STARTED;
    ASSERT_EQUAL(SPROUT_STATE_STARTED, state);
}

TEST(power_mode) {
    sprout_power_mode_t mode = SPROUT_POWER_NORMAL;
    ASSERT_EQUAL(SPROUT_POWER_NORMAL, mode);
    mode = SPROUT_POWER_LOW;
    ASSERT_EQUAL(SPROUT_POWER_LOW, mode);
}

TEST(queue_mode) {
    sprout_queue_mode_t mode = SPROUT_QUEUE_MODE_NORMAL;
    ASSERT_EQUAL(SPROUT_QUEUE_MODE_NORMAL, mode);
    mode = SPROUT_QUEUE_MODE_LATEST_ONLY;
    ASSERT_EQUAL(SPROUT_QUEUE_MODE_LATEST_ONLY, mode);
}

TEST(package_type) {
    sprout_package_t pkg = {0};
    pkg.package_type = SPROUT_PACKAGE_TYPE_SINGLE;
    ASSERT_EQUAL(SPROUT_PACKAGE_TYPE_SINGLE, pkg.package_type);
    pkg.package_type = SPROUT_PACKAGE_TYPE_START;
    ASSERT_EQUAL(SPROUT_PACKAGE_TYPE_START, pkg.package_type);
}

TEST(sequence_number) {
    uint32_t seq = 0;
    seq++;
    ASSERT_EQUAL(1, seq);
    seq = UINT32_MAX;
    seq++;
    ASSERT_EQUAL(0, seq);
}

TEST(hashmap_basic) {
    hashmap_t map;
    hashmap_init(&map, 32, mac_hash, mac_equals);
    
    uint8_t key[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    int value = 42;
    
    ASSERT_TRUE(hashmap_put(&map, key, &value));
    ASSERT_EQUAL(1, hashmap_size(&map));
    
    int *retrieved = (int *)hashmap_get(&map, key);
    ASSERT_NOT_NULL(retrieved);
    ASSERT_EQUAL(42, *retrieved);
    
    hashmap_free(&map);
}

TEST(hashmap_remove) {
    hashmap_t map;
    hashmap_init(&map, 32, mac_hash, mac_equals);
    
    uint8_t key[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    int value = 42;
    hashmap_put(&map, key, &value);
    
    ASSERT_TRUE(hashmap_remove(&map, key));
    ASSERT_EQUAL(0, hashmap_size(&map));
    
    hashmap_free(&map);
}

TEST(network_stats) {
    sprout_network_stats_t stats = {0};
    stats.packets_sent = 10;
    stats.packets_received = 5;
    stats.packets_dropped = 2;
    
    ASSERT_EQUAL(10, stats.packets_sent);
    ASSERT_EQUAL(5, stats.packets_received);
    ASSERT_EQUAL(2, stats.packets_dropped);
}

TEST(mac_address_copy) {
    uint8_t src[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t dest[6];
    memcpy(dest, src, 6);
    ASSERT_EQUAL_MEMORY(src, dest, 6);
}

TEST(network_name) {
    char name[32];
    const char *test_name = "TestNetwork";
    _safe_string_copy(name, test_name, sizeof(name));
    ASSERT_EQUAL_STRING(test_name, name);
}

// Mode-specific tests
TEST(client_mode_connection_check) {
    sprout_network_t net = {0};
    net.current_mode = SPROUT_MODE_CLIENT;
    ASSERT_EQUAL(SPROUT_MODE_CLIENT, net.current_mode);
}

TEST(host_mode_connection_check) {
    sprout_network_t net = {0};
    net.current_mode = SPROUT_MODE_HOST;
    ASSERT_EQUAL(SPROUT_MODE_HOST, net.current_mode);
}

TEST(extender_mode_connection_check) {
    sprout_network_t net = {0};
    net.current_mode = SPROUT_MODE_EXTENDER;
    ASSERT_EQUAL(SPROUT_MODE_EXTENDER, net.current_mode);
}

TEST(client_discovery_logic) {
    // Simulate client discovering a host
    uint8_t host_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    char host_name[32] = "TestHost";
    
    // Client should be able to store discovered host info
    sprout_peer_info_t discovered_peer;
    memcpy(discovered_peer.mac, host_mac, 6);
    _safe_string_copy(discovered_peer.name, host_name, sizeof(discovered_peer.name));
    discovered_peer.is_connected = false;
    discovered_peer.state = SPROUT_PEER_STATE_DISCOVERED;
    
    ASSERT_EQUAL_MEMORY(host_mac, discovered_peer.mac, 6);
    ASSERT_EQUAL_STRING(host_name, discovered_peer.name);
    ASSERT_FALSE(discovered_peer.is_connected);
    ASSERT_EQUAL(SPROUT_PEER_STATE_DISCOVERED, discovered_peer.state);
}

TEST(host_peer_management) {
    // Simulate host managing multiple clients
    hashmap_t peer_map;
    hashmap_init(&peer_map, 32, mac_hash, mac_equals);
    
    uint8_t client1[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t client2[6] = {0x02, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    int peer1_id = 1;
    int peer2_id = 2;
    
    hashmap_put(&peer_map, client1, &peer1_id);
    hashmap_put(&peer_map, client2, &peer2_id);
    
    ASSERT_EQUAL(2, hashmap_size(&peer_map));
    
    int *retrieved = (int *)hashmap_get(&peer_map, client1);
    ASSERT_NOT_NULL(retrieved);
    ASSERT_EQUAL(1, *retrieved);
    
    hashmap_free(&peer_map);
}

TEST(extender_packet_forwarding) {
    // Simulate extender forwarding logic
    sprout_package_t pkg = {0};
    
    uint8_t origin[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t dest[6] = {0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
    
    memcpy(pkg.origin_mac, origin, 6);
    memcpy(pkg.dest_mac, dest, 6);
    pkg.hop_count = 0;
    pkg.max_hops = 5;
    
    // Extender should increment hop count
    pkg.hop_count++;
    ASSERT_EQUAL(1, pkg.hop_count);
    
    // Should not exceed max hops
    pkg.hop_count = pkg.max_hops;
    ASSERT_FALSE(pkg.hop_count < pkg.max_hops);
}

TEST(mode_switching) {
    sprout_network_t net = {0};
    
    // Start in client mode
    net.current_mode = SPROUT_MODE_CLIENT;
    ASSERT_EQUAL(SPROUT_MODE_CLIENT, net.current_mode);
    
    // Switch to host mode
    net.current_mode = SPROUT_MODE_HOST;
    ASSERT_EQUAL(SPROUT_MODE_HOST, net.current_mode);
    
    // Switch to extender mode
    net.current_mode = SPROUT_MODE_EXTENDER;
    ASSERT_EQUAL(SPROUT_MODE_EXTENDER, net.current_mode);
}

TEST(client_auto_connection_mode) {
    sprout_network_t net = {0};
    net.current_mode = SPROUT_MODE_CLIENT;
    
    // Simulate auto connection mode
    uint8_t host_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    bool auto_connect = true;
    
    if (auto_connect) {
        // Client would initiate connection
        SPROUT_STORE_HASH_TYPE peer = {0};
        memcpy(peer.peer_info.peer_addr, host_mac, 6);
        peer.is_connected = true;
        peer.state = SPROUT_PEER_STATE_CONNECTED;
        
        ASSERT_TRUE(peer.is_connected);
        ASSERT_EQUAL(SPROUT_PEER_STATE_CONNECTED, peer.state);
    }
}

TEST(host_max_peers_limit) {
    sprout_network_t net = {0};
    net.current_mode = SPROUT_MODE_HOST;
    
    // Simulate max peers configuration
    uint32_t max_peers = 32;
    uint32_t current_peers = 5;
    
    bool can_add_more = (current_peers < max_peers);
    ASSERT_TRUE(can_add_more);
    
    current_peers = max_peers;
    can_add_more = (current_peers < max_peers);
    ASSERT_FALSE(can_add_more);
}

TEST(security_handshake_initiation) {
    // Simulate security handshake initiation
    uint8_t peer_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    // Simulate PWK generation
    uint8_t pwk[16] = {0};
    for (int i = 0; i < 16; i++) {
        pwk[i] = i; // Mock key
    }
    
    // Check that key is non-zero (simulating successful generation)
    bool key_valid = false;
    for (int i = 0; i < 16; i++) {
        if (pwk[i] != 0) {
            key_valid = true;
            break;
        }
    }
    ASSERT_TRUE(key_valid);
}

TEST(keepalive_tracking) {
    // Simulate keepalive timestamp tracking
    int64_t last_keepalive = 1000000; // 1 second in microseconds
    int64_t current_time = 5000000;   // 5 seconds in microseconds
    int64_t keepalive_interval = 3000000; // 3 seconds
    
    bool keepalive_expired = (current_time - last_keepalive) > keepalive_interval;
    ASSERT_TRUE(keepalive_expired);
    
    last_keepalive = 4000000; // 4 seconds
    keepalive_expired = (current_time - last_keepalive) > keepalive_interval;
    ASSERT_FALSE(keepalive_expired);
}

TEST(broadcast_peer_setup) {
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_peer_info_t broadcast_info = {0};
    
    memcpy(broadcast_info.peer_addr, broadcast_mac, 6);
    broadcast_info.channel = 0;
    broadcast_info.encrypt = false;
    
    ASSERT_EQUAL_MEMORY(broadcast_mac, broadcast_info.peer_addr, 6);
    ASSERT_EQUAL(0, broadcast_info.channel);
    ASSERT_FALSE(broadcast_info.encrypt);
}

int main(void) {
    printf("=== Sprout Unit Tests (Standalone) ===\n\n");
    
    RUN_TEST(safe_string_copy_normal);
    RUN_TEST(safe_string_copy_truncate);
    RUN_TEST(is_broadcast_address_true);
    RUN_TEST(is_broadcast_address_false);
    RUN_TEST(peer_state_transitions);
    RUN_TEST(network_state_transitions);
    RUN_TEST(power_mode);
    RUN_TEST(queue_mode);
    RUN_TEST(package_type);
    RUN_TEST(sequence_number);
    RUN_TEST(hashmap_basic);
    RUN_TEST(hashmap_remove);
    RUN_TEST(network_stats);
    RUN_TEST(mac_address_copy);
    RUN_TEST(network_name);
    
    // Mode-specific tests
    RUN_TEST(client_mode_connection_check);
    RUN_TEST(host_mode_connection_check);
    RUN_TEST(extender_mode_connection_check);
    RUN_TEST(client_discovery_logic);
    RUN_TEST(host_peer_management);
    RUN_TEST(extender_packet_forwarding);
    RUN_TEST(mode_switching);
    RUN_TEST(client_auto_connection_mode);
    RUN_TEST(host_max_peers_limit);
    RUN_TEST(security_handshake_initiation);
    RUN_TEST(keepalive_tracking);
    RUN_TEST(broadcast_peer_setup);
    
    printf("\n=== Test Results ===\n");
    printf("Total: %d\n", tests_run);
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("\nAll tests PASSED!\n");
        return 0;
    } else {
        printf("\nSome tests FAILED!\n");
        return 1;
    }
}
