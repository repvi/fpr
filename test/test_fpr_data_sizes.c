/**
 * @file test_fpr_data_sizes.c
 * @brief FPR Data Size Test Implementation
 * 
 * Tests various payload sizes: 50, 100, 150, 200, 250, 300, 350, 400, 450, 500,
 * 600, 700, 750, 800, 850, 900, 950, 1000 bytes
 */

#include "test_fpr_data_sizes.h"
#include "fpr/fpr.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_random.h"

static const char *TAG = "FPR_DATA_SIZE_TEST";

// Test payload sizes (in bytes)
static const uint16_t TEST_SIZES[] = {
    50, 100, 150, 200, 250, 300, 350, 400, 450, 500,
    600, 700, 750, 800, 850, 900, 950, 1000
};
static const size_t NUM_TEST_SIZES = sizeof(TEST_SIZES) / sizeof(TEST_SIZES[0]);

// Test configuration
static bool test_auto_mode = true;
static uint32_t test_interval_ms = 2000;
static uint32_t test_rx_timeout_ms = 5000;
static bool test_echo_mode = true;
static bool is_host_mode = false;

// Statistics
static uint32_t tests_passed = 0;
static uint32_t tests_failed = 0;
static uint32_t bytes_sent = 0;
static uint32_t bytes_received = 0;

// Task handles
static TaskHandle_t test_task_handle = NULL;
static TaskHandle_t stats_task_handle = NULL;

// Connection state
static bool is_connected = false;
static uint8_t peer_mac[6];

// Forward declarations
static void generate_test_payload(uint8_t *buffer, uint16_t size, uint16_t test_id);

/**
 * Wait for complete data reception with timeout
 * Polls fpr_network_get_data_from_peer until data is available or timeout
 */
static bool wait_for_data(uint8_t *peer_mac, void *buffer, int size, TickType_t total_timeout_ms, TickType_t poll_interval_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(total_timeout_ms);
    TickType_t poll_ticks = pdMS_TO_TICKS(poll_interval_ms);
    
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        // Try to get data with short timeout per attempt
        bool got_data = fpr_network_get_data_from_peer(peer_mac, buffer, size, poll_ticks);
        if (got_data) {
            return true;
        }
        
        // Small delay between polling attempts
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    return false; // Timeout
}

/**
 * Generate test payload with pattern
 */
static void generate_test_payload(uint8_t *buffer, uint16_t size, uint16_t test_id)
{
    // Header: test_id (2 bytes) + size (2 bytes)
    buffer[0] = (test_id >> 8) & 0xFF;
    buffer[1] = test_id & 0xFF;
    buffer[2] = (size >> 8) & 0xFF;
    buffer[3] = size & 0xFF;
    
    // Fill remaining with deterministic pattern based on test_id
    // This allows verification to regenerate the exact same pattern
    uint32_t seed = 0xA5A5A5A5 ^ (test_id * 0x12345678);
    for (uint16_t i = 4; i < size; i++) {
        buffer[i] = (uint8_t)((i + seed) & 0xFF);
    }
}

/**
 * Verify test payload with strict byte-by-byte comparison
 */
static bool verify_test_payload(const uint8_t *buffer, uint16_t expected_size, uint16_t expected_test_id)
{
    if (buffer == NULL) {
        ESP_LOGE(TAG, "[VERIFY] NULL buffer");
        return false;
    }
    
    // Check header
    uint16_t test_id = (buffer[0] << 8) | buffer[1];
    uint16_t size = (buffer[2] << 8) | buffer[3];
    
    if (test_id != expected_test_id) {
        ESP_LOGE(TAG, "[VERIFY] Test ID mismatch: expected %u, got %u", expected_test_id, test_id);
        return false;
    }
    
    if (size != expected_size) {
        ESP_LOGE(TAG, "[VERIFY] Size mismatch: expected %u, got %u", expected_size, size);
        return false;
    }
    
#ifdef CONFIG_FPR_DATA_SIZE_TEST_VERIFY_PAYLOAD
    // Strict byte-by-byte verification
    // Regenerate expected pattern and compare
    uint8_t *expected_buffer = heap_caps_malloc(expected_size, MALLOC_CAP_DEFAULT);
    if (!expected_buffer) {
        ESP_LOGE(TAG, "[VERIFY] Failed to allocate verification buffer");
        return false;
    }
    
    // Regenerate the same pattern that was sent
    generate_test_payload(expected_buffer, expected_size, expected_test_id);
    
    // Compare byte-by-byte
    bool match = true;
    for (uint16_t i = 0; i < expected_size; i++) {
        if (buffer[i] != expected_buffer[i]) {
            ESP_LOGE(TAG, "[VERIFY] Byte mismatch at offset %u: expected 0x%02X, got 0x%02X",
                     i, expected_buffer[i], buffer[i]);
            match = false;
            // Show first 5 mismatches only
            static int mismatch_count = 0;
            if (++mismatch_count >= 5) {
                ESP_LOGE(TAG, "[VERIFY] ... (stopping after 5 mismatches)");
                break;
            }
        }
    }
    
    heap_caps_free(expected_buffer);
    
    if (!match) {
        return false;
    }
#endif
    
    ESP_LOGI(TAG, "[VERIFY] ✓ Payload verified: test_id=%u, size=%u bytes", test_id, size);
    return true;
}

/**
 * Data receive callback - NOT USED
 * Using direct polling with fpr_network_get_data_from_peer instead
 */
static void on_data_received(void *peer_addr, void *data, void *user_data)
{
    (void)peer_addr;
    (void)data;
    (void)user_data;
    // Callback not used - we poll directly with fpr_network_get_data_from_peer
}

/**
 * Client test task - sends all test sizes
 */
static void client_test_task(void *pvParameters)
{
    (void)pvParameters;
    
    ESP_LOGI(TAG, "[CLIENT] Waiting for connection...");
    
    // Wait for connection
    while (!is_connected) {
        if (fpr_client_is_connected()) {
            is_connected = true;
            esp_err_t err = fpr_client_get_host_info(peer_mac, NULL, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "[CLIENT] Failed to get host info: %s", esp_err_to_name(err));
                vTaskDelete(NULL);
                return;
            }
            ESP_LOGI(TAG, "[CLIENT] Connected to host " MACSTR, MAC2STR(peer_mac));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Run through all test sizes
    for (size_t i = 0; i < NUM_TEST_SIZES; i++) {
        uint16_t size = TEST_SIZES[i];
        uint16_t test_id = (uint16_t)(i + 1);
        
        // Allocate buffer
        uint8_t *buffer = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
        if (!buffer) {
            ESP_LOGE(TAG, "[CLIENT] Failed to allocate %u bytes", size);
            tests_failed++;
            continue;
        }
        
        // Generate test payload
        generate_test_payload(buffer, size, test_id);
        
        ESP_LOGI(TAG, "[CLIENT] Sending test #%u: %u bytes...", test_id, size);
        esp_err_t err = fpr_network_send_to_peer(peer_mac, buffer, size, -1);
        
        if (err == ESP_OK) {
            bytes_sent += size;
            ESP_LOGI(TAG, "[CLIENT] ✓ Test #%u sent successfully (%u bytes)", test_id, size);
            
            // If echo mode, wait for response
            if (test_echo_mode) {
                ESP_LOGI(TAG, "[CLIENT] Waiting for echo response (timeout: %lu ms)...", test_rx_timeout_ms);
                uint8_t *rx_buffer = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
                if (rx_buffer) {
                    // Use wait_for_data with polling to ensure complete reception
                    bool got_data = wait_for_data(peer_mac, rx_buffer, size, test_rx_timeout_ms, 100);
                    if (got_data) {
                        bytes_received += size;
                        ESP_LOGI(TAG, "[CLIENT] Received echo response (%u bytes)", size);
                        
                        // Verify the echo matches what we sent (byte-by-byte comparison)
                        bool match = true;
                        for (uint16_t i = 0; i < size; i++) {
                            if (rx_buffer[i] != buffer[i]) {
                                ESP_LOGE(TAG, "[CLIENT] Echo byte mismatch at offset %u: sent 0x%02X, received 0x%02X",
                                         i, buffer[i], rx_buffer[i]);
                                match = false;
                                // Show first 5 mismatches only
                                static int echo_mismatch_count = 0;
                                if (++echo_mismatch_count >= 5) {
                                    ESP_LOGE(TAG, "[CLIENT] ... (stopping after 5 mismatches)");
                                    break;
                                }
                            }
                        }
                        
                        if (match) {
                            tests_passed++;
                            ESP_LOGI(TAG, "[CLIENT] ✓ Echo verified for test #%u (exact match)", test_id);
                        } else {
                            tests_failed++;
                            ESP_LOGE(TAG, "[CLIENT] ✗ Echo verification failed for test #%u (data mismatch)", test_id);
                        }
                    } else {
                        tests_failed++;
                        ESP_LOGE(TAG, "[CLIENT] ✗ No echo response for test #%u (timeout: %lu ms)", test_id, test_rx_timeout_ms);
                    }
                    heap_caps_free(rx_buffer);
                } else {
                    tests_failed++;
                    ESP_LOGE(TAG, "[CLIENT] ✗ Failed to allocate rx buffer for test #%u", test_id);
                }
            } else {
                tests_passed++;
            }
        } else {
            tests_failed++;
            ESP_LOGE(TAG, "[CLIENT] ✗ Test #%u send failed: %s", test_id, esp_err_to_name(err));
        }
        
        heap_caps_free(buffer);
        
        // Wait before next test
        vTaskDelay(pdMS_TO_TICKS(test_interval_ms));
    }
    
    ESP_LOGI(TAG, "[CLIENT] All tests completed. Passed: %lu, Failed: %lu", tests_passed, tests_failed);
    test_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * Host test task - polls for incoming data
 */
static void host_test_task(void *pvParameters)
{
    (void)pvParameters;
    
    ESP_LOGI(TAG, "[HOST] Waiting for client connections and data...");
    
    // Continuously poll for incoming data from any connected client
    while (1) {
        // Wait a bit for connection
        size_t connected = fpr_host_get_connected_count();
        if (connected == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        
        // Get list of connected clients
        fpr_peer_info_t peers[5];
        size_t peer_count = fpr_list_all_peers(peers, 5);
        
        for (size_t i = 0; i < peer_count; i++) {
            if (peers[i].state != FPR_PEER_STATE_CONNECTED) {
                continue;
            }
            
            // Try to receive data from this peer (use max test size as buffer)
            uint8_t *rx_buffer = heap_caps_malloc(1000, MALLOC_CAP_DEFAULT);
            if (!rx_buffer) {
                ESP_LOGE(TAG, "[HOST] Failed to allocate rx buffer");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            
            // Poll with configurable timeout to allow fragmented packets to complete
            bool got_data = fpr_network_get_data_from_peer(peers[i].mac, rx_buffer, 1000, pdMS_TO_TICKS(test_rx_timeout_ms));
            
            if (got_data) {
                // Extract header
                uint16_t test_id = (rx_buffer[0] << 8) | rx_buffer[1];
                uint16_t size = (rx_buffer[2] << 8) | rx_buffer[3];
                
                bytes_received += size;
                ESP_LOGI(TAG, "[HOST] Received %u bytes from " MACSTR " (test_id=%u)",
                         size, MAC2STR(peers[i].mac), test_id);
                
                // Generate expected payload to compare byte-by-byte
                uint8_t *expected_buffer = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
                if (expected_buffer) {
                    generate_test_payload(expected_buffer, size, test_id);
                    
                    // Compare received data with expected data
                    bool match = true;
                    for (uint16_t j = 0; j < size; j++) {
                        if (rx_buffer[j] != expected_buffer[j]) {
                            ESP_LOGE(TAG, "[HOST] Data mismatch at offset %u: expected 0x%02X, received 0x%02X",
                                     j, expected_buffer[j], rx_buffer[j]);
                            match = false;
                            // Show first 5 mismatches only
                            static int host_mismatch_count = 0;
                            if (++host_mismatch_count >= 5) {
                                ESP_LOGE(TAG, "[HOST] ... (stopping after 5 mismatches)");
                                break;
                            }
                        }
                    }
                    
                    if (match) {
                        tests_passed++;
                        ESP_LOGI(TAG, "[HOST] ✓ Test #%u PASSED (%u bytes, exact match)", test_id, size);
                    } else {
                        tests_failed++;
                        ESP_LOGE(TAG, "[HOST] ✗ Test #%u FAILED (%u bytes, data mismatch)", test_id, size);
                    }
                    
                    heap_caps_free(expected_buffer);
                } else {
                    tests_failed++;
                    ESP_LOGE(TAG, "[HOST] ✗ Failed to allocate expected buffer for test #%u", test_id);
                }
                
                // Echo back if echo mode enabled
                if (test_echo_mode) {
                    ESP_LOGI(TAG, "[HOST] Sending %u bytes back to client...", size);
                    esp_err_t err = fpr_network_send_to_peer(peers[i].mac, rx_buffer, size, -1);
                    if (err == ESP_OK) {
                        bytes_sent += size;
                        ESP_LOGI(TAG, "[HOST] Echo sent successfully");
                    } else {
                        ESP_LOGE(TAG, "[HOST] Echo failed: %s", esp_err_to_name(err));
                    }
                }
            }
            
            heap_caps_free(rx_buffer);
        }
        
        // Small delay between polling cycles
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Periodic status update
        static TickType_t last_status = 0;
        if ((xTaskGetTickCount() - last_status) > pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "[HOST] Connected clients: %zu, Tests passed: %lu, Failed: %lu",
                     connected, tests_passed, tests_failed);
            last_status = xTaskGetTickCount();
        }
    }
}

/**
 * Statistics task
 */
static void stats_task(void *pvParameters)
{
    (void)pvParameters;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        
        ESP_LOGI(TAG, "=== DATA SIZE TEST STATS ===");
        ESP_LOGI(TAG, "  Tests Passed:    %lu", tests_passed);
        ESP_LOGI(TAG, "  Tests Failed:    %lu", tests_failed);
        ESP_LOGI(TAG, "  Bytes Sent:      %lu", bytes_sent);
        ESP_LOGI(TAG, "  Bytes Received:  %lu", bytes_received);
        ESP_LOGI(TAG, "===========================");
        
        fpr_network_stats_t net_stats;
        fpr_get_network_stats(&net_stats);
        ESP_LOGI(TAG, "  FPR Stats:");
        ESP_LOGI(TAG, "    Packets Sent:      %lu", net_stats.packets_sent);
        ESP_LOGI(TAG, "    Packets Received:  %lu", net_stats.packets_received);
        ESP_LOGI(TAG, "    Packets Dropped:   %lu", net_stats.packets_dropped);
        ESP_LOGI(TAG, "    Send Failures:     %lu", net_stats.send_failures);
    }
}

/**
 * Initialize WiFi
 */
static esp_err_t init_wifi(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

// ========== PUBLIC API ==========

esp_err_t fpr_data_size_test_host_start(const fpr_data_size_test_config_t *config)
{
    is_host_mode = true;
    
    // Apply config (use Kconfig defaults if not provided)
    if (config) {
        test_auto_mode = config->auto_mode;
        test_interval_ms = config->test_interval_ms > 0 ? config->test_interval_ms : 2000;
        test_echo_mode = config->echo_mode;
    } else {
#ifdef CONFIG_FPR_DATA_SIZE_TEST_AUTO_MODE
        test_auto_mode = true;
#else
        test_auto_mode = false;
#endif
#ifdef CONFIG_FPR_DATA_SIZE_TEST_ECHO_MODE
        test_echo_mode = true;
#else
        test_echo_mode = false;
#endif
#ifdef CONFIG_FPR_DATA_SIZE_TEST_INTERVAL_MS
        test_interval_ms = CONFIG_FPR_DATA_SIZE_TEST_INTERVAL_MS;
#else
        test_interval_ms = 2000;
#endif
#ifdef CONFIG_FPR_DATA_SIZE_TEST_RX_TIMEOUT_MS
        test_rx_timeout_ms = CONFIG_FPR_DATA_SIZE_TEST_RX_TIMEOUT_MS;
#else
        test_rx_timeout_ms = 5000;
#endif
    }
    
    ESP_LOGI(TAG, "Starting DATA SIZE TEST - HOST mode");
    ESP_LOGI(TAG, "  Auto mode:       %s", test_auto_mode ? "YES" : "NO");
    ESP_LOGI(TAG, "  Echo mode:       %s", test_echo_mode ? "YES" : "NO");
    ESP_LOGI(TAG, "  RX timeout:      %lu ms", test_rx_timeout_ms);
    
    // Initialize WiFi
    ESP_ERROR_CHECK(init_wifi());
    
    // Initialize FPR
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[32];
    snprintf(name, sizeof(name), "fpr-host-%02X%02X", mac[4], mac[5]);
    
    ESP_ERROR_CHECK(fpr_network_init(name));
    
    // Configure host
    fpr_host_config_t host_cfg = {
        .max_peers = 5,
        .connection_mode = test_auto_mode ? FPR_CONNECTION_AUTO : FPR_CONNECTION_MANUAL,
        .request_cb = NULL
    };
    ESP_ERROR_CHECK(fpr_host_set_config(&host_cfg));
    
    // Note: NOT registering receive callback - using direct polling instead
    // fpr_register_receive_callback(on_data_received);
    
    // Start FPR
    ESP_ERROR_CHECK(fpr_network_start());
    fpr_network_set_mode(FPR_MODE_HOST);
    
    // Start host loop
    ESP_ERROR_CHECK(fpr_network_start_loop_task(pdMS_TO_TICKS(60000), false));
    
    // Start test task
    xTaskCreate(host_test_task, "host_test", 4096, NULL, 5, &test_task_handle);
    xTaskCreate(stats_task, "stats", 4096, NULL, 3, &stats_task_handle);
    
    ESP_LOGI(TAG, "HOST test started successfully");
    return ESP_OK;
}

esp_err_t fpr_data_size_test_client_start(const fpr_data_size_test_config_t *config)
{
    is_host_mode = false;
    
    // Apply config (use Kconfig defaults if not provided)
    if (config) {
        test_auto_mode = config->auto_mode;
        test_interval_ms = config->test_interval_ms > 0 ? config->test_interval_ms : 2000;
        test_echo_mode = config->echo_mode;
    } else {
#ifdef CONFIG_FPR_DATA_SIZE_TEST_AUTO_MODE
        test_auto_mode = true;
#else
        test_auto_mode = false;
#endif
#ifdef CONFIG_FPR_DATA_SIZE_TEST_ECHO_MODE
        test_echo_mode = true;
#else
        test_echo_mode = false;
#endif
#ifdef CONFIG_FPR_DATA_SIZE_TEST_INTERVAL_MS
        test_interval_ms = CONFIG_FPR_DATA_SIZE_TEST_INTERVAL_MS;
#else
        test_interval_ms = 2000;
#endif
#ifdef CONFIG_FPR_DATA_SIZE_TEST_RX_TIMEOUT_MS
        test_rx_timeout_ms = CONFIG_FPR_DATA_SIZE_TEST_RX_TIMEOUT_MS;
#else
        test_rx_timeout_ms = 5000;
#endif
    }
    
    ESP_LOGI(TAG, "Starting DATA SIZE TEST - CLIENT mode");
    ESP_LOGI(TAG, "  Auto mode:       %s", test_auto_mode ? "YES" : "NO");
    ESP_LOGI(TAG, "  Echo mode:       %s", test_echo_mode ? "YES" : "NO");
    ESP_LOGI(TAG, "  Test interval:   %lu ms", test_interval_ms);
    ESP_LOGI(TAG, "  RX timeout:      %lu ms", test_rx_timeout_ms);
    
    // Initialize WiFi
    ESP_ERROR_CHECK(init_wifi());
    
    // Initialize FPR
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[32];
    snprintf(name, sizeof(name), "fpr-client-%02X%02X", mac[4], mac[5]);
    
    ESP_ERROR_CHECK(fpr_network_init(name));
    
    // Configure client
    fpr_client_config_t client_cfg = {
        .connection_mode = test_auto_mode ? FPR_CONNECTION_AUTO : FPR_CONNECTION_MANUAL,
        .discovery_cb = NULL,
        .selection_cb = NULL
    };
    ESP_ERROR_CHECK(fpr_client_set_config(&client_cfg));
    
    // Note: NOT registering receive callback - using direct polling instead
    // fpr_register_receive_callback(on_data_received);
    
    // Start FPR
    ESP_ERROR_CHECK(fpr_network_start());
    fpr_network_set_mode(FPR_MODE_CLIENT);
    
    // Start client loop
    ESP_ERROR_CHECK(fpr_network_start_loop_task(pdMS_TO_TICKS(30000), false));
    
    // Start test task
    xTaskCreate(client_test_task, "client_test", 8192, NULL, 5, &test_task_handle);
    xTaskCreate(stats_task, "stats", 4096, NULL, 3, &stats_task_handle);
    
    ESP_LOGI(TAG, "CLIENT test started successfully");
    return ESP_OK;
}

void fpr_data_size_test_stop(void)
{
    if (test_task_handle) {
        vTaskDelete(test_task_handle);
        test_task_handle = NULL;
    }
    if (stats_task_handle) {
        vTaskDelete(stats_task_handle);
        stats_task_handle = NULL;
    }
    
    fpr_network_stop();
    ESP_LOGI(TAG, "Test stopped");
}

void fpr_data_size_test_get_stats(uint32_t *tests_passed_out, uint32_t *tests_failed_out,
                                   uint32_t *bytes_sent_out, uint32_t *bytes_received_out)
{
    if (tests_passed_out) *tests_passed_out = tests_passed;
    if (tests_failed_out) *tests_failed_out = tests_failed;
    if (bytes_sent_out) *bytes_sent_out = bytes_sent;
    if (bytes_received_out) *bytes_received_out = bytes_received;
}
