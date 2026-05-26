#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Mock ESP-IDF types and functions for unit testing

// ESP error codes
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_INVALID_STATE -4
#define ESP_ERR_NOT_FOUND -5
#define ESP_ERR_NOT_SUPPORTED -6
#define ESP_ERR_TIMEOUT -7
#define ESP_ERR_ESPNOW_NO_MEM -1000

// MAC address length
#define MAC_ADDRESS_LENGTH 6
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

// ESP-NOW types
typedef struct {
    uint8_t peer_addr[6];
    uint8_t channel;
    uint8_t encrypt;
    uint8_t lmk[16];
    int8_t rssi;
} esp_now_peer_info_t;

typedef struct {
    uint8_t src_addr[6];
    void *rx_ctrl;
} esp_now_recv_info_t;

typedef struct {
    int8_t rssi;
} wifi_rx_ctrl_t;

typedef struct {
    int8_t rssi;
} wifi_tx_info_t;

typedef enum {
    ESP_NOW_SEND_SUCCESS = 0,
    ESP_NOW_SEND_FAIL = 1
} esp_now_send_status_t;

// Mock functions
void mock_esp_now_init(void);
void mock_esp_now_deinit(void);
void mock_esp_now_register_send_cb(void);
void mock_esp_now_register_recv_cb(void);
void mock_esp_now_add_peer(const uint8_t *mac);
void mock_esp_now_del_peer(const uint8_t *mac);
void mock_esp_now_send(const uint8_t *dest_addr, const void *data, size_t len);

// Test control
void mock_esp_now_reset(void);
void mock_esp_now_set_send_result(int result);
void mock_esp_now_set_recv_callback(void (*cb)(const esp_now_recv_info_t*, const void*, size_t));
