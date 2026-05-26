#include "esp_mock.h"
#include <string.h>
#include <stdlib.h>

static int mock_send_result = ESP_OK;
static void (*mock_recv_callback)(const esp_now_recv_info_t*, const void*, size_t) = NULL;
static bool esp_now_initialized = false;

void mock_esp_now_init(void) {
    esp_now_initialized = true;
}

void mock_esp_now_deinit(void) {
    esp_now_initialized = false;
}

void mock_esp_now_register_send_cb(void) {
    // Mock - no action needed
}

void mock_esp_now_register_recv_cb(void) {
    // Mock - no action needed
}

void mock_esp_now_add_peer(const uint8_t *mac) {
    // Mock - track peer if needed
}

void mock_esp_now_del_peer(const uint8_t *mac) {
    // Mock - remove peer if tracked
}

void mock_esp_now_send(const uint8_t *dest_addr, const void *data, size_t len) {
    // Mock - could trigger callback if needed
}

void mock_esp_now_reset(void) {
    mock_send_result = ESP_OK;
    mock_recv_callback = NULL;
    esp_now_initialized = false;
}

void mock_esp_now_set_send_result(int result) {
    mock_send_result = result;
}

void mock_esp_now_set_recv_callback(void (*cb)(const esp_now_recv_info_t*, const void*, size_t)) {
    mock_recv_callback = cb;
}
