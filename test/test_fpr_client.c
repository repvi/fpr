/**
 * FPR Client Test Library
 * 
 * Tests the FPR library in Client mode with both automatic and manual connection modes.
 * This test connects to a host and sends periodic test messages.
 * 
 * Usage from main.cpp:
 *   #include "test_fpr_client.h"
 *   
 *   fpr_client_test_config_t config = {
 *       .auto_mode = true,
 *       .scan_duration_ms = 5000,
 *       .message_interval_ms = 5000
 *   };
 *   fpr_client_test_start(&config);
 */

#include "test_fpr_client.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fpr/fpr.h"
#include "esp_mac.h"

static const char *TAG = "FPR_CLIENT_TEST";

// Test configuration (set via fpr_client_test_start)
static bool test_auto_mode = true;
static uint32_t test_scan_duration_ms = 5000;
static uint32_t test_message_interval_ms = 5000;

// Connection state
static bool is_connected = false;
static uint8_t connected_host_mac[6];
static char connected_host_name[32];

// Statistics
static uint32_t hosts_found = 0;
static uint32_t messages_sent = 0;
static uint32_t messages_received = 0;
static uint32_t connection_attempts = 0;
static uint32_t successful_connections = 0;
static uint32_t reconnection_attempts = 0;
static uint32_t successful_reconnections = 0;
static uint32_t connection_drops = 0;

// Task handles
static TaskHandle_t stats_task_handle = NULL;
static TaskHandle_t message_task_handle = NULL;
static TaskHandle_t manual_conn_task_handle = NULL;
static TaskHandle_t auto_connect_task_handle = NULL;

/**
 * Client callback: Host discovered
 */
static void client_on_host_discovered(const uint8_t *host_mac, const char *host_name, int8_t rssi)
{
    hosts_found++;
    ESP_LOGI(TAG, "[DISCOVERY] Host found #%lu: %s (" MACSTR ") RSSI: %d dBm",
             hosts_found, host_name, MAC2STR(host_mac), rssi);
}

/**
 * Application data callback
 */
static void client_on_data_received(void *peer_addr, void *data, void *user_data)
{
    messages_received++;
    
    uint8_t *src_mac = (uint8_t *)peer_addr;
    int len = user_data ? *((int *)user_data) : 0;
    
    // Update last_seen timestamp by getting peer info (triggers internal update)
    fpr_peer_info_t peer_info;
    if (fpr_get_peer_info(src_mac, &peer_info) == ESP_OK) {
        ESP_LOGD(TAG, "[DATA] Host %s last seen: %llu ms ago", peer_info.name, peer_info.last_seen_ms);
    }
    
    ESP_LOGI(TAG, "[DATA] Message #%lu from " MACSTR " (size: %d bytes)",
             messages_received, MAC2STR(src_mac), len);
    
    // Print data as hex and ASCII
    uint8_t *bytes = (uint8_t *)data;
    printf("  HEX: ");
    for (int i = 0; i < len; i++) {
        printf("%02X ", bytes[i]);
    }
    printf("\n  ASCII: ");
    for (int i = 0; i < len; i++) {
        printf("%c", (bytes[i] >= 32 && bytes[i] < 127) ? bytes[i] : '.');
    }
    printf("\n");
}

/**
 * Send test message to host
 */
static void send_test_message(void)
{
    if (!is_connected) {
        ESP_LOGW(TAG, "[SEND] Not connected to any host, skipping message send");
        return;
    }
    
    // Get current host MAC (in case it changed or wasn't set yet)
    esp_err_t info_err = fpr_client_get_host_info(connected_host_mac, connected_host_name, sizeof(connected_host_name));
    if (info_err != ESP_OK) {
        ESP_LOGE(TAG, "[SEND] Failed to get host info: %s", esp_err_to_name(info_err));
        return;
    }
    
    // Create test message with counter
    char message[45];
    snprintf(message, sizeof(message), "Test message #%lu from client", messages_sent + 1);
    
    ESP_LOGI(TAG, "[SEND] Sending message to %s: \"%s\"", connected_host_name, message);
    esp_err_t err = fpr_network_send_to_peer(connected_host_mac, (uint8_t *)message, strlen(message) + 1, -1);
    
    if (err == ESP_OK) {
        messages_sent++;
        ESP_LOGI(TAG, "[SEND] Message sent successfully (total: %lu)", messages_sent);
    } else {
        ESP_LOGE(TAG, "[SEND] Failed to send message: %s", esp_err_to_name(err));
    }
}

/**
 * Client loop task - runs discovery loop for 20 seconds
 */
static void client_loop_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[LOOP] Starting client discovery loop for 20 seconds...");
    
    // Start the client loop (20 seconds)
    esp_err_t err = fpr_network_start_loop_task(pdMS_TO_TICKS(20000), false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[LOOP] Failed to start loop task: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    
    // Wait for loop to complete
    while (fpr_network_is_loop_task_running()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    ESP_LOGI(TAG, "[LOOP] Discovery loop completed");
    
    // Start persistent reconnect monitoring
    ESP_LOGI(TAG, "[RECONNECT] Starting persistent reconnect task...");
    esp_err_t reconnect_err = fpr_network_start_reconnect_task();
    if (reconnect_err == ESP_OK) {
        ESP_LOGI(TAG, "[RECONNECT] Reconnect task started - connections will be maintained indefinitely");
    } else {
        ESP_LOGE(TAG, "[RECONNECT] Failed to start reconnect task: %s", esp_err_to_name(reconnect_err));
    }
    
    // Check if connected
    is_connected = fpr_client_is_connected();
    if (is_connected) {
        successful_connections++;
        esp_err_t info_err = fpr_client_get_host_info(connected_host_mac, connected_host_name, sizeof(connected_host_name));
        if (info_err == ESP_OK) {
            ESP_LOGI(TAG, "[LOOP] Successfully connected to host: %s (" MACSTR ")", 
                     connected_host_name, MAC2STR(connected_host_mac));
        }
    } else {
        ESP_LOGW(TAG, "[LOOP] Loop completed but no connection established");
    }
    
    // Now wait for incoming data
    ESP_LOGI(TAG, "[LOOP] Waiting for data from host...");
    
    while (1) {
        if (is_connected) {
            // Try to receive data from host
            uint8_t buffer[200];
            if (fpr_network_get_data_from_peer(connected_host_mac, buffer, 200, pdMS_TO_TICKS(1000))) {
                messages_received++;
                ESP_LOGI(TAG, "[RECEIVE] Got data from host: %s", (char*)buffer);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}



/**
 * Message sending task
 */
static void message_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    const uint32_t TEST_MESSAGE_INTERVAL_MS = test_message_interval_ms;
    
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TEST_MESSAGE_INTERVAL_MS));
        
        if (is_connected) {
            send_test_message();
        }
    }
}

/**
 * Statistics task
 */
static void stats_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    bool was_connected = false;
    
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10000)); // Every 10 seconds
        
        // Update connection status
        bool currently_connected = fpr_client_is_connected();
        
        // Detect connection state changes
        if (currently_connected && !was_connected) {
            if (successful_connections > 0) {
                successful_reconnections++;
                ESP_LOGI(TAG, "[RECONNECT] Successfully reconnected! (reconnection #%lu)", successful_reconnections);
            }
        } else if (!currently_connected && was_connected) {
            connection_drops++;
            ESP_LOGW(TAG, "[DISCONNECT] Connection dropped! (drop #%lu)", connection_drops);
        }
        
        is_connected = currently_connected;
        was_connected = currently_connected;
        
        ESP_LOGI(TAG, "========== STATISTICS ==========");
        ESP_LOGI(TAG, "Mode: %s", test_auto_mode ? "AUTO" : "MANUAL");
        ESP_LOGI(TAG, "Connected: %s", is_connected ? "YES" : "NO");
        if (is_connected) {
            ESP_LOGI(TAG, "Host: %s (" MACSTR ")", connected_host_name, MAC2STR(connected_host_mac));
            
            // Get host info
            uint8_t host_mac[6];
            char host_name[32];
            if (fpr_client_get_host_info(host_mac, host_name, sizeof(host_name)) == ESP_OK) {
                ESP_LOGI(TAG, "Verified: %s", host_name);
            }
        }
        ESP_LOGI(TAG, "Hosts found: %lu", hosts_found);
        ESP_LOGI(TAG, "Connection attempts: %lu", connection_attempts);
        ESP_LOGI(TAG, "Successful connections: %lu", successful_connections);
        ESP_LOGI(TAG, "Reconnection attempts: %lu", reconnection_attempts);
        ESP_LOGI(TAG, "Successful reconnections: %lu", successful_reconnections);
        ESP_LOGI(TAG, "Connection drops: %lu", connection_drops);
        ESP_LOGI(TAG, "Messages sent: %lu", messages_sent);
        ESP_LOGI(TAG, "Messages received: %lu", messages_received);
        ESP_LOGI(TAG, "================================");
    }
}

/**
 * Initialize WiFi
 */
static esp_err_t wifi_init(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) return ret;
    
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) return ret;
    
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) return ret;
    
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;
    
    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;
    
    ESP_LOGI(TAG, "WiFi initialized in station mode");
    return ESP_OK;
}

/**
 * Monitor task - runs in main loop
 */
static void monitor_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Every minute
        
        if (is_connected) {
            ESP_LOGI(TAG, "[MONITOR] Still connected to: %s", connected_host_name);
        } else {
            ESP_LOGW(TAG, "[MONITOR] Not connected to any host");
        }
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

esp_err_t fpr_client_test_start(const fpr_client_test_config_t *config)
{
    // Set configuration
    if (config != NULL) {
        test_auto_mode = config->auto_mode;
        test_scan_duration_ms = config->scan_duration_ms;
        test_message_interval_ms = config->message_interval_ms;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "FPR Client Test Starting");
    ESP_LOGI(TAG, "Mode: %s", test_auto_mode ? "AUTOMATIC" : "MANUAL");
    ESP_LOGI(TAG, "Message Interval: %lu ms", test_message_interval_ms);
    ESP_LOGI(TAG, "========================================");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize WiFi
    ret = wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize FPR
    ESP_LOGI(TAG, "Initializing FPR network...");
    ret = fpr_network_init("FPR-Client-Test");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FPR init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "FPR network initialized");
    
    // Configure as client
    fpr_client_config_t client_config = {
        .connection_mode = test_auto_mode ? FPR_CONNECTION_AUTO : FPR_CONNECTION_MANUAL,
        .discovery_cb = client_on_host_discovered
    };
    
    ret = fpr_client_set_config(&client_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set client config: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Client configuration set");
    
    // Register data receive callback
    fpr_register_receive_callback(client_on_data_received);
    
    // Start the network
    ESP_LOGI(TAG, "Starting FPR network...");
    ret = fpr_network_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start network: %s", esp_err_to_name(ret));
        return ret;
    }
    // Set mode to client (after ESP-NOW is initialized)
    fpr_network_set_mode(FPR_MODE_CLIENT);
    ESP_LOGI(TAG, "Mode set to CLIENT");
    
    // Check connection status periodically
    is_connected = fpr_client_is_connected();
    if (is_connected) {
        esp_err_t info_err = fpr_client_get_host_info(connected_host_mac, connected_host_name, sizeof(connected_host_name));
        if (info_err == ESP_OK) {
            successful_connections++;
            ESP_LOGI(TAG, "[CONNECT] Already connected to: %s (" MACSTR ")", connected_host_name, MAC2STR(connected_host_mac));
        }
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "FPR Client is now RUNNING");
    if (test_auto_mode) {
        ESP_LOGI(TAG, "Waiting for automatic host connection...");
    } else {
        ESP_LOGI(TAG, "Starting manual host discovery...");
    }
    ESP_LOGI(TAG, "========================================");
    
    // Start tasks
    xTaskCreate(stats_task, "client_stats", 4096, NULL, 5, &stats_task_handle);
    xTaskCreate(message_task, "client_msg", 4096, NULL, 5, &message_task_handle);
    xTaskCreate(monitor_task, "client_mon", 4096, NULL, 5, NULL);
    xTaskCreate(client_loop_task, "client_loop", 4096, NULL, 5, &auto_connect_task_handle);
    
    return ESP_OK;
}

void fpr_client_test_stop(void)
{
    if (stats_task_handle != NULL) {
        vTaskDelete(stats_task_handle);
        stats_task_handle = NULL;
    }
    
    if (message_task_handle != NULL) {
        vTaskDelete(message_task_handle);
        message_task_handle = NULL;
    }
    
    if (auto_connect_task_handle != NULL) {
        vTaskDelete(auto_connect_task_handle);
        auto_connect_task_handle = NULL;
    }
    
    if (manual_conn_task_handle != NULL) {
        vTaskDelete(manual_conn_task_handle);
        manual_conn_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "FPR Client Test stopped");
}

void fpr_client_test_get_stats(bool *connected, uint32_t *hosts, 
                                uint32_t *msgs_sent, uint32_t *msgs_recv)
{
    if (connected) *connected = is_connected;
    if (hosts) *hosts = hosts_found;
    if (msgs_sent) *msgs_sent = messages_sent;
    if (msgs_recv) *msgs_recv = messages_received;
}

bool fpr_client_test_is_connected(void)
{
    return is_connected;
}
