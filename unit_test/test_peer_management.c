#include "unity.h"
#include "../internal_include/sprout/internal/private_defs.h"
#include "../internal_include/sprout/internal/helpers.h"
#include "../lib/hashmap.h"
#include "../lib/hashmap_presets.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>

// Mock global network state
sprout_network_t sprout_net = {0};

void setUp(void) {
    memset(&sprout_net, 0, sizeof(sprout_net));
    sprout_net.peers_map = malloc(sizeof(struct hashmap));
    hashmap_init(sprout_net.peers_map, 32, mac_hash, mac_equals);
}

void tearDown(void) {
    hashmap_free(sprout_net.peers_map);
    free(sprout_net.peers_map);
    
    if (sprout_net.allowlist_map) {
        hashmap_free(sprout_net.allowlist_map);
        free(sprout_net.allowlist_map);
    }
    
    memset(&sprout_net, 0, sizeof(sprout_net));
}

void test_add_peer_internal_basic(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    esp_err_t err = _add_peer_internal(test_mac, "TestPeer", true, 0);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    TEST_ASSERT_NOT_NULL(peer);
    TEST_ASSERT_EQUAL_STRING("TestPeer", peer->name);
    TEST_ASSERT_TRUE(peer->is_connected);
}

void test_add_peer_internal_null_mac(void) {
    esp_err_t err = _add_peer_internal(NULL, "TestPeer", true, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

void test_add_peer_internal_duplicate(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    _add_peer_internal(test_mac, "TestPeer1", true, 0);
    esp_err_t err = _add_peer_internal(test_mac, "TestPeer2", true, 0);
    // Should fail or overwrite depending on implementation
    TEST_ASSERT(err != ESP_OK);
}

void test_remove_peer_internal(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    _add_peer_internal(test_mac, "TestPeer", true, 0);

    esp_err_t err = _remove_peer_internal(test_mac);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    TEST_ASSERT_NULL(peer);
}

void test_remove_peer_internal_not_found(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    esp_err_t err = _remove_peer_internal(test_mac);
    TEST_ASSERT_EQUAL(ESP_OK, err); // remove should succeed even if not found
}

void test_copy_peer_to_info(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    _add_peer_internal(test_mac, "TestPeer", true, 0);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    sprout_peer_info_t info;
    _copy_peer_to_info(peer, &info);

    TEST_ASSERT_EQUAL_MEMORY(test_mac, info.mac, 6);
    TEST_ASSERT_EQUAL_STRING("TestPeer", info.name);
    TEST_ASSERT_TRUE(info.is_connected);
}

void test_add_discovered_peer(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    esp_err_t err = _add_discovered_peer("DiscoveredPeer", test_mac, 0, false);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    TEST_ASSERT_NOT_NULL(peer);
    TEST_ASSERT_EQUAL_STRING("DiscoveredPeer", peer->name);
    TEST_ASSERT_FALSE(peer->is_connected);
}

void test_peer_count(void) {
    uint8_t mac1[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t mac2[6] = {0x02, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t mac3[6] = {0x03, 0x02, 0x03, 0x04, 0x05, 0x06};

    _add_peer_internal(mac1, "Peer1", true, 0);
    _add_peer_internal(mac2, "Peer2", true, 0);
    _add_peer_internal(mac3, "Peer3", true, 0);

    size_t count = hashmap_size(sprout_net.peers_map);
    TEST_ASSERT_EQUAL(3, count);
}

void test_block_peer(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    _add_peer_internal(test_mac, "TestPeer", true, 0);

    esp_err_t err = sprout_host_block_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    TEST_ASSERT_NOT_NULL(peer);
    TEST_ASSERT_EQUAL(SPROUT_PEER_STATE_BLOCKED, peer->state);
    TEST_ASSERT_FALSE(peer->is_connected);
}

void test_block_peer_null_mac(void) {
    esp_err_t err = sprout_host_block_peer(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

void test_block_peer_not_in_map(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    // Block peer that doesn't exist yet - should add it as blocked
    esp_err_t err = sprout_host_block_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    TEST_ASSERT_NOT_NULL(peer);
    TEST_ASSERT_EQUAL(SPROUT_PEER_STATE_BLOCKED, peer->state);
}

void test_unblock_peer(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    _add_peer_internal(test_mac, "TestPeer", true, 0);
    sprout_host_block_peer(test_mac);

    esp_err_t err = sprout_host_unblock_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    TEST_ASSERT_NOT_NULL(peer);
    TEST_ASSERT_EQUAL(SPROUT_PEER_STATE_DISCOVERED, peer->state);
}

void test_unblock_peer_not_found(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    esp_err_t err = sprout_host_unblock_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, err);
}

void test_unblock_peer_not_blocked(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    _add_peer_internal(test_mac, "TestPeer", true, 0);

    esp_err_t err = sprout_host_unblock_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);
}

void test_allow_peer(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    esp_err_t err = sprout_host_allow_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Verify it's in the allowlist
    TEST_ASSERT_TRUE(sprout_net.allowlist_map != NULL);
    TEST_ASSERT_TRUE(hashmap_get(sprout_net.allowlist_map, test_mac) != NULL);
}

void test_allow_peer_null_mac(void) {
    esp_err_t err = sprout_host_allow_peer(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

void test_allow_peer_duplicate(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    sprout_host_allow_peer(test_mac);
    esp_err_t err = sprout_host_allow_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);  // Already in allowlist
}

void test_disallow_peer(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    sprout_host_allow_peer(test_mac);
    esp_err_t err = sprout_host_disallow_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Verify it's removed from allowlist
    TEST_ASSERT_TRUE(hashmap_get(sprout_net.allowlist_map, test_mac) == NULL);
}

void test_disallow_peer_not_found(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    
    esp_err_t err = sprout_host_disallow_peer(test_mac);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, err);
}

void test_disallow_peer_null_mac(void) {
    esp_err_t err = sprout_host_disallow_peer(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

void test_enable_allowlist(void) {
    esp_err_t err = sprout_host_enable_allowlist();
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_TRUE(sprout_host_is_allowlist_enabled());
    TEST_ASSERT_TRUE(sprout_net.allowlist_map != NULL);
}

void test_disable_allowlist(void) {
    sprout_host_enable_allowlist();
    
    esp_err_t err = sprout_host_disable_allowlist();
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_FALSE(sprout_host_is_allowlist_enabled());
}

void test_is_allowlist_enabled(void) {
    TEST_ASSERT_FALSE(sprout_host_is_allowlist_enabled());
    
    sprout_host_enable_allowlist();
    TEST_ASSERT_TRUE(sprout_host_is_allowlist_enabled());
    
    sprout_host_disable_allowlist();
    TEST_ASSERT_FALSE(sprout_host_is_allowlist_enabled());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add_peer_internal_basic);
    RUN_TEST(test_add_peer_internal_null_mac);
    RUN_TEST(test_add_peer_internal_duplicate);
    RUN_TEST(test_remove_peer_internal);
    RUN_TEST(test_remove_peer_internal_not_found);
    RUN_TEST(test_copy_peer_to_info);
    RUN_TEST(test_add_discovered_peer);
    RUN_TEST(test_peer_count);
    RUN_TEST(test_block_peer);
    RUN_TEST(test_block_peer_null_mac);
    RUN_TEST(test_block_peer_not_in_map);
    RUN_TEST(test_unblock_peer);
    RUN_TEST(test_unblock_peer_not_found);
    RUN_TEST(test_unblock_peer_not_blocked);
    RUN_TEST(test_allow_peer);
    RUN_TEST(test_allow_peer_null_mac);
    RUN_TEST(test_allow_peer_duplicate);
    RUN_TEST(test_disallow_peer);
    RUN_TEST(test_disallow_peer_not_found);
    RUN_TEST(test_disallow_peer_null_mac);
    RUN_TEST(test_enable_allowlist);
    RUN_TEST(test_disable_allowlist);
    RUN_TEST(test_is_allowlist_enabled);
    return UNITY_END();
}
