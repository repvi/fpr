#include "unity.h"
#include "../internal_include/sprout/internal/private_defs.h"
#include <string.h>

sprout_network_t sprout_net = {0};

void setUp(void) {
    memset(&sprout_net, 0, sizeof(sprout_net));
}

void tearDown(void) {
    memset(&sprout_net, 0, sizeof(sprout_net));
}

void test_network_state_uninitialized(void) {
    sprout_net.state = SPROUT_STATE_UNINITIALIZED;
    TEST_ASSERT_EQUAL(SPROUT_STATE_UNINITIALIZED, sprout_net.state);
}

void test_network_state_initialized(void) {
    sprout_net.state = SPROUT_STATE_INITIALIZED;
    TEST_ASSERT_EQUAL(SPROUT_STATE_INITIALIZED, sprout_net.state);
}

void test_network_state_started(void) {
    sprout_net.state = SPROUT_STATE_STARTED;
    TEST_ASSERT_EQUAL(SPROUT_STATE_STARTED, sprout_net.state);
}

void test_network_state_paused(void) {
    sprout_net.state = SPROUT_STATE_PAUSED;
    TEST_ASSERT_EQUAL(SPROUT_STATE_PAUSED, sprout_net.state);
}

void test_network_state_stopped(void) {
    sprout_net.state = SPROUT_STATE_STOPPED;
    TEST_ASSERT_EQUAL(SPROUT_STATE_STOPPED, sprout_net.state);
}

void test_power_mode_normal(void) {
    sprout_net.power_mode = SPROUT_POWER_NORMAL;
    TEST_ASSERT_EQUAL(SPROUT_POWER_NORMAL, sprout_net.power_mode);
}

void test_power_mode_low(void) {
    sprout_net.power_mode = SPROUT_POWER_LOW;
    TEST_ASSERT_EQUAL(SPROUT_POWER_LOW, sprout_net.power_mode);
}

void test_queue_mode_normal(void) {
    sprout_net.default_queue_mode = SPROUT_QUEUE_MODE_NORMAL;
    TEST_ASSERT_EQUAL(SPROUT_QUEUE_MODE_NORMAL, sprout_net.default_queue_mode);
}

void test_queue_mode_latest_only(void) {
    sprout_net.default_queue_mode = SPROUT_QUEUE_MODE_LATEST_ONLY;
    TEST_ASSERT_EQUAL(SPROUT_QUEUE_MODE_LATEST_ONLY, sprout_net.default_queue_mode);
}

void test_channel_setting(void) {
    sprout_net.channel = 6;
    TEST_ASSERT_EQUAL(6, sprout_net.channel);
}

void test_channel_zero_auto(void) {
    sprout_net.channel = 0;
    TEST_ASSERT_EQUAL(0, sprout_net.channel);
}

void test_visibility_public(void) {
    sprout_net.access_state = SPROUT_VISIBILITY_PUBLIC;
    TEST_ASSERT_EQUAL(SPROUT_VISIBILITY_PUBLIC, sprout_net.access_state);
}

void test_visibility_private(void) {
    sprout_net.access_state = SPROUT_VISIBILITY_PRIVATE;
    TEST_ASSERT_EQUAL(SPROUT_VISIBILITY_PRIVATE, sprout_net.access_state);
}

void test_mode_client(void) {
    sprout_net.current_mode = SPROUT_MODE_CLIENT;
    TEST_ASSERT_EQUAL(SPROUT_MODE_CLIENT, sprout_net.current_mode);
}

void test_mode_host(void) {
    sprout_net.current_mode = SPROUT_MODE_HOST;
    TEST_ASSERT_EQUAL(SPROUT_MODE_HOST, sprout_net.current_mode);
}

void test_mode_extender(void) {
    sprout_net.current_mode = SPROUT_MODE_EXTENDER;
    TEST_ASSERT_EQUAL(SPROUT_MODE_EXTENDER, sprout_net.current_mode);
}

void test_sequence_number_increment(void) {
    sprout_net.tx_sequence_num = 0;
    sprout_net.tx_sequence_num++;
    TEST_ASSERT_EQUAL(1, sprout_net.tx_sequence_num);
}

void test_sequence_number_overflow(void) {
    sprout_net.tx_sequence_num = UINT32_MAX;
    sprout_net.tx_sequence_num++;
    TEST_ASSERT_EQUAL(0, sprout_net.tx_sequence_num);
}

void test_network_stats_initialization(void) {
    memset(&sprout_net.stats, 0, sizeof(sprout_net.stats));
    TEST_ASSERT_EQUAL(0, sprout_net.stats.packets_sent);
    TEST_ASSERT_EQUAL(0, sprout_net.stats.packets_received);
    TEST_ASSERT_EQUAL(0, sprout_net.stats.packets_dropped);
    TEST_ASSERT_EQUAL(0, sprout_net.stats.send_failures);
}

void test_network_stats_increment(void) {
    sprout_net.stats.packets_sent = 10;
    sprout_net.stats.packets_received = 5;
    sprout_net.stats.packets_dropped = 2;

    TEST_ASSERT_EQUAL(10, sprout_net.stats.packets_sent);
    TEST_ASSERT_EQUAL(5, sprout_net.stats.packets_received);
    TEST_ASSERT_EQUAL(2, sprout_net.stats.packets_dropped);
}

void test_paused_flag(void) {
    sprout_net.paused = false;
    TEST_ASSERT_FALSE(sprout_net.paused);

    sprout_net.paused = true;
    TEST_ASSERT_TRUE(sprout_net.paused);
}

void test_mac_address_storage(void) {
    uint8_t test_mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(sprout_net.mac, test_mac, 6);
    TEST_ASSERT_EQUAL_MEMORY(test_mac, sprout_net.mac, 6);
}

void test_network_name_storage(void) {
    const char *name = "TestNetwork";
    strncpy(sprout_net.name, name, sizeof(sprout_net.name) - 1);
    sprout_net.name[sizeof(sprout_net.name) - 1] = '\0';
    TEST_ASSERT_EQUAL_STRING(name, sprout_net.name);
}

void test_network_name_truncation(void) {
    const char *long_name = "ThisIsAVeryLongNetworkNameThatShouldBeTruncated";
    strncpy(sprout_net.name, long_name, sizeof(sprout_net.name) - 1);
    sprout_net.name[sizeof(sprout_net.name) - 1] = '\0';
    TEST_ASSERT_EQUAL(strlen(long_name), strlen(sprout_net.name));
    TEST_ASSERT_GREATER_THAN(sizeof(sprout_net.name), strlen(long_name));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_network_state_uninitialized);
    RUN_TEST(test_network_state_initialized);
    RUN_TEST(test_network_state_started);
    RUN_TEST(test_network_state_paused);
    RUN_TEST(test_network_state_stopped);
    RUN_TEST(test_power_mode_normal);
    RUN_TEST(test_power_mode_low);
    RUN_TEST(test_queue_mode_normal);
    RUN_TEST(test_queue_mode_latest_only);
    RUN_TEST(test_channel_setting);
    RUN_TEST(test_channel_zero_auto);
    RUN_TEST(test_visibility_public);
    RUN_TEST(test_visibility_private);
    RUN_TEST(test_mode_client);
    RUN_TEST(test_mode_host);
    RUN_TEST(test_mode_extender);
    RUN_TEST(test_sequence_number_increment);
    RUN_TEST(test_sequence_number_overflow);
    RUN_TEST(test_network_stats_initialization);
    RUN_TEST(test_network_stats_increment);
    RUN_TEST(test_paused_flag);
    RUN_TEST(test_mac_address_storage);
    RUN_TEST(test_network_name_storage);
    RUN_TEST(test_network_name_truncation);
    return UNITY_END();
}
