#include "unity.h"
#include "../internal_include/sprout/internal/private_defs.h"
#include "../internal_include/sprout/internal/helpers.h"
#include <string.h>
#include <stdlib.h>

sprout_network_t sprout_net = {0};

void setUp(void) {
    memset(&sprout_net, 0, sizeof(sprout_net));
    hashmap_init(&sprout_net.peers_map, 32, mac_hash, mac_equals);
    sprout_net.default_queue_mode = SPROUT_QUEUE_MODE_NORMAL;
}

void tearDown(void) {
    hashmap_free(&sprout_net.peers_map);
    memset(&sprout_net, 0, sizeof(sprout_net));
}

void test_package_type_single(void) {
    sprout_package_t pkg = {0};
    pkg.package_type = SPROUT_PACKAGE_TYPE_SINGLE;
    pkg.payload_size = 10;
    pkg.sequence_num = 1;

    TEST_ASSERT_EQUAL(SPROUT_PACKAGE_TYPE_SINGLE, pkg.package_type);
    TEST_ASSERT_EQUAL(10, pkg.payload_size);
    TEST_ASSERT_EQUAL(1, pkg.sequence_num);
}

void test_package_type_fragmented(void) {
    sprout_package_t pkg_start = {0};
    pkg_start.package_type = SPROUT_PACKAGE_TYPE_START;
    pkg_start.sequence_num = 1;

    sprout_package_t pkg_cont = {0};
    pkg_cont.package_type = SPROUT_PACKAGE_TYPE_CONTINUED;
    pkg_cont.sequence_num = 1;

    sprout_package_t pkg_end = {0};
    pkg_end.package_type = SPROUT_PACKAGE_TYPE_END;
    pkg_end.sequence_num = 1;

    TEST_ASSERT_EQUAL(SPROUT_PACKAGE_TYPE_START, pkg_start.package_type);
    TEST_ASSERT_EQUAL(SPROUT_PACKAGE_TYPE_CONTINUED, pkg_cont.package_type);
    TEST_ASSERT_EQUAL(SPROUT_PACKAGE_TYPE_END, pkg_end.package_type);
    TEST_ASSERT_EQUAL(pkg_start.sequence_num, pkg_cont.sequence_num);
    TEST_ASSERT_EQUAL(pkg_cont.sequence_num, pkg_end.sequence_num);
}

void test_package_size_compatibility(void) {
    TEST_ASSERT_TRUE(is_sprout_package_compatible(sizeof(sprout_package_t)));
    TEST_ASSERT_FALSE(is_sprout_package_compatible(sizeof(sprout_package_t) - 1));
    TEST_ASSERT_FALSE(is_sprout_package_compatible(sizeof(sprout_package_t) + 1));
}

void test_package_mac_addresses(void) {
    sprout_package_t pkg = {0};
    uint8_t origin[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t dest[6] = {0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};

    memcpy(pkg.origin_mac, origin, 6);
    memcpy(pkg.dest_mac, dest, 6);

    TEST_ASSERT_EQUAL_MEMORY(origin, pkg.origin_mac, 6);
    TEST_ASSERT_EQUAL_MEMORY(dest, pkg.dest_mac, 6);
}

void test_package_hop_count(void) {
    sprout_package_t pkg = {0};
    pkg.hop_count = 0;
    pkg.max_hops = 5;

    TEST_ASSERT_EQUAL(0, pkg.hop_count);
    TEST_ASSERT_EQUAL(5, pkg.max_hops);
}

void test_package_id_control(void) {
    sprout_package_t pkg = {0};
    pkg.id = SPROUT_PACKET_ID_CONTROL;

    TEST_ASSERT_EQUAL(SPROUT_PACKET_ID_CONTROL, pkg.id);
}

void test_package_version(void) {
    sprout_package_t pkg = {0};
    pkg.version = SPROUT_NETWORK_VERSION;

    TEST_ASSERT_EQUAL(SPROUT_NETWORK_VERSION, pkg.version);
}

void test_package_payload_size_limit(void) {
    sprout_package_t pkg = {0};
    const size_t max_payload = sizeof(pkg.protocol);

    pkg.payload_size = max_payload;
    TEST_ASSERT_EQUAL(max_payload, pkg.payload_size);

    pkg.payload_size = max_payload + 1;
    TEST_ASSERT_GREATER_THAN(max_payload, pkg.payload_size);
}

void test_queue_mode_normal(void) {
    sprout_queue_mode_t mode = SPROUT_QUEUE_MODE_NORMAL;
    TEST_ASSERT_EQUAL(SPROUT_QUEUE_MODE_NORMAL, mode);
}

void test_queue_mode_latest_only(void) {
    sprout_queue_mode_t mode = SPROUT_QUEUE_MODE_LATEST_ONLY;
    TEST_ASSERT_EQUAL(SPROUT_QUEUE_MODE_LATEST_ONLY, mode);
}

void test_peer_state_transitions(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    _add_peer_internal(test_mac, "TestPeer", false, 0);

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(test_mac);
    TEST_ASSERT_NOT_NULL(peer);
    TEST_ASSERT_EQUAL(SPROUT_PEER_STATE_DISCOVERED, peer->state);

    peer->state = SPROUT_PEER_STATE_CONNECTED;
    TEST_ASSERT_EQUAL(SPROUT_PEER_STATE_CONNECTED, peer->state);

    peer->state = SPROUT_PEER_STATE_BLOCKED;
    TEST_ASSERT_EQUAL(SPROUT_PEER_STATE_BLOCKED, peer->state);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_package_type_single);
    RUN_TEST(test_package_type_fragmented);
    RUN_TEST(test_package_size_compatibility);
    RUN_TEST(test_package_mac_addresses);
    RUN_TEST(test_package_hop_count);
    RUN_TEST(test_package_id_control);
    RUN_TEST(test_package_version);
    RUN_TEST(test_package_payload_size_limit);
    RUN_TEST(test_queue_mode_normal);
    RUN_TEST(test_queue_mode_latest_only);
    RUN_TEST(test_peer_state_transitions);
    return UNITY_END();
}
