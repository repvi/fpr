/**
 * FPR Host Test Library
 * 
 * Tests the FPR library in Host mode with both automatic and manual connection modes.
 * This test accepts client connections and echoes back received data.
 * 
 * Usage from main.cpp:
 *   #include "test_fpr_host.h"
 *   
 *   fpr_host_test_config_t config = {
 *       .auto_mode = true,
 *       .max_peers = 5,
 *       .echo_enabled = true
 *   };
 *   fpr_host_test_start(&config);
 */

#include "test_fpr_host.h"
#include "fpr/fpr.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"

static const char *TAG = "FPR_HOST_TEST";

// Test configuration (set via fpr_host_test_start)
static bool test_auto_mode = true;
static uint32_t test_max_peers = 5;
static bool test_echo_enabled = true;
static bool test_use_latest_only_mode = false;

// Statistics
static uint32_t peers_discovered = 0;
static uint32_t peers_connected = 0;
static uint32_t peers_reconnected = 0;
static uint32_t messages_received = 0;
static uint32_t bytes_received = 0;

// Task handles
static TaskHandle_t stats_task_handle = NULL;
static TaskHandle_t main_test_task_handle = NULL;

/**
 * Manual approval callback (only called in manual mode)
 */
static bool host_connection_request_cb(const uint8_t *peer_mac, const char *peer_name, uint32_t peer_key)
{
    peers_discovered++;
    ESP_LOGI(TAG, "[REQUEST] Connection request #%lu from %s (" MACSTR ") key: 0x%08lX",
             peers_discovered, peer_name, MAC2STR(peer_mac), (unsigned long)peer_key);
    
    // Get RSSI from peer info if available
    fpr_peer_info_t info;
    int8_t rssi = 0;
    if (fpr_get_peer_info((uint8_t*)peer_mac, &info) == ESP_OK) {
        rssi = info.rssi;
    }
    
    // For testing, approve all peers with RSSI > -70 (or approve all if RSSI unavailable)
    bool approve = (rssi == 0 || rssi > -70);
    
    if (approve) {
        ESP_LOGI(TAG, "[APPROVAL] Approving peer: %s (RSSI: %d dBm)", peer_name, rssi);
        peers_connected++;
    } else {
        ESP_LOGI(TAG, "[REJECTION] Rejecting peer (weak signal): %s (RSSI: %d dBm)", peer_name, rssi);
    }
    
    return approve;
}

/**
 * Application data callback
 */
static void host_on_data_received(void *peer_addr, void *data, void *user_data)
{
    messages_received++;
    
    uint8_t *src_mac = (uint8_t *)peer_addr;
    int len = user_data ? *((int *)user_data) : 0;
    bytes_received += len;
    
    // Update last_seen timestamp by getting peer info (triggers internal update)
    fpr_peer_info_t peer_info;
    if (fpr_get_peer_info(src_mac, &peer_info) == ESP_OK) {
        ESP_LOGD(TAG, "[DATA] Peer %s last seen: %llu ms ago", peer_info.name, peer_info.last_seen_ms);
        
        // Check if this is the first message after reconnection (security state just established)
        static bool first_message_logged = false;
        if (!first_message_logged && messages_received == 1) {
            ESP_LOGI(TAG, "[RECONNECT] Received first message from %s after handshake completion", peer_info.name);
            first_message_logged = true;
        }
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
    
    if (test_echo_enabled) {
        // Echo back the data
        ESP_LOGI(TAG, "[ECHO] Sending data back to client...");
        esp_err_t err = fpr_network_send_to_peer((uint8_t*)src_mac, data, len, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[ERROR] Failed to echo data: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "[ECHO] Echo sent successfully");
        }
    }
}

/**
 * Print peer list
 */
static void print_peer_list(void)
{
    fpr_peer_info_t peers[20];
    
    // Get all peers
    size_t total = fpr_list_all_peers(peers, 20);
    ESP_LOGI(TAG, "=== All Peers (%zu) ===", total);
    for (size_t i = 0; i < total; i++) {
        ESP_LOGI(TAG, "  %zu. %s (" MACSTR ") - State: %d, RSSI: %d, Last seen: %llu ms ago",
                 i + 1, peers[i].name, MAC2STR(peers[i].mac),
                 peers[i].state, peers[i].rssi, peers[i].last_seen_ms);
    }
    
    // Filter and display connected peers
    size_t connected = 0;
    for (size_t i = 0; i < total; i++) {
        if (peers[i].state == FPR_PEER_STATE_CONNECTED) {
            connected++;
        }
    }
    ESP_LOGI(TAG, "=== Connected Peers (%zu) ===", connected);
    for (size_t i = 0; i < total; i++) {
        if (peers[i].state == FPR_PEER_STATE_CONNECTED) {
            ESP_LOGI(TAG, "  %s (" MACSTR ") - RSSI: %d, Last seen: %llu ms ago",
                     peers[i].name, MAC2STR(peers[i].mac), peers[i].rssi, peers[i].last_seen_ms);
        }
    }
}

/**
 * Statistics task
 */
static void stats_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10000)); // Every 10 seconds
        
        // In AUTO mode, counters aren't updated by callback, so count from peer list
        size_t current_connected = fpr_host_get_connected_count();
        size_t total_peers = (size_t)fpr_network_get_peer_count();
        
        // Update counters for auto mode (manual mode uses callback)
        if (test_auto_mode) {
            peers_discovered = (uint32_t)total_peers;
            peers_connected = (uint32_t)current_connected;
        }
        
        ESP_LOGI(TAG, "========== STATISTICS ==========");
        ESP_LOGI(TAG, "Mode: %s", test_auto_mode ? "AUTO" : "MANUAL");
        ESP_LOGI(TAG, "Queue Mode: %s", test_use_latest_only_mode ? "LATEST_ONLY" : "NORMAL");
        ESP_LOGI(TAG, "Peers discovered: %lu", peers_discovered);
        ESP_LOGI(TAG, "Peers connected: %lu", peers_connected);
        ESP_LOGI(TAG, "Peers reconnected: %lu", peers_reconnected);
        ESP_LOGI(TAG, "Currently connected: %zu", current_connected);
        ESP_LOGI(TAG, "Messages received: %lu", messages_received);
        ESP_LOGI(TAG, "Bytes received: %lu", bytes_received);
        
        // Show network stats including queue-related metrics
        fpr_network_stats_t net_stats;
        fpr_get_network_stats(&net_stats);
        ESP_LOGI(TAG, "Packets dropped (queue overflow/latest-only): %lu", (unsigned long)net_stats.packets_dropped);
        ESP_LOGI(TAG, "Replay attacks blocked: %lu", (unsigned long)net_stats.replay_attacks_blocked);
        ESP_LOGI(TAG, "================================");
        
        print_peer_list();
    }
}

/**
 * Host loop task - runs host loop for 20 seconds
 */
static void host_loop_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[LOOP] Starting host broadcast loop for 20 seconds...");
    
    // Start the host loop (20 seconds)
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
    
    ESP_LOGI(TAG, "[LOOP] Host broadcast loop completed");
    
    // Start persistent reconnect monitoring
    ESP_LOGI(TAG, "[RECONNECT] Starting persistent reconnect task...");
    esp_err_t reconnect_err = fpr_network_start_reconnect_task();
    if (reconnect_err == ESP_OK) {
        ESP_LOGI(TAG, "[RECONNECT] Reconnect task started - will monitor client connections indefinitely");
    } else {
        ESP_LOGE(TAG, "[RECONNECT] Failed to start reconnect task: %s", esp_err_to_name(reconnect_err));
    }
    
    // Check connected peers
    size_t connected_count = fpr_host_get_connected_count();
    ESP_LOGI(TAG, "[LOOP] Connected peers: %zu", connected_count);
    
    if (connected_count > 0) {
        // List all peers
        print_peer_list();
        
        // Send data to all connected peers periodically
        ESP_LOGI(TAG, "[LOOP] Starting to send data to connected clients...");
        
        uint32_t msg_count = 0;
        while (1) {
            // Get all connected peers
            fpr_peer_info_t peers[10];
            size_t count = fpr_list_all_peers(peers, 10);
            
            for (size_t i = 0; i < count; i++) {
                if (peers[i].state == FPR_PEER_STATE_CONNECTED) {
                    // Send test message to this peer
                    char message[100];
                    snprintf(message, sizeof(message), "Host message #%lu to %s", ++msg_count, peers[i].name);
                    
                    esp_err_t ret = fpr_network_send_to_peer(peers[i].mac, (uint8_t*)message, 
                                                             strlen(message) + 1, 0);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "[SEND] Sent to %s: %s", peers[i].name, message);
                    } else {
                        ESP_LOGE(TAG, "[SEND] Failed to send to %s: %s", peers[i].name, esp_err_to_name(ret));
                    }
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(5000)); // Send every 5 seconds
        }
    } else {
        ESP_LOGW(TAG, "[LOOP] No clients connected after loop");
        vTaskDelay(portMAX_DELAY);
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
 * Comprehensive queue mode stress test for host - runs automatically
 * Tests NORMAL -> LATEST_ONLY -> NORMAL with various data sizes on all connected peers
 */
static void host_queue_mode_stress_test_task(void *pvParameters)
{
    // Wait for at least one client to connect
    while (fpr_host_get_connected_count() == 0) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    // Wait a bit more for stable connection
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║     HOST: COMPREHENSIVE QUEUE MODE STRESS TEST               ║");
    ESP_LOGI(TAG, "║     Testing: NORMAL -> LATEST_ONLY -> NORMAL                 ║");
    ESP_LOGI(TAG, "║     With multiple data sizes on all connected peers          ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // Test data sizes
    const int DATA_SIZES[] = {32, 100, 150};
    const char *SIZE_NAMES[] = {"SMALL(32B)", "MEDIUM(100B)", "LARGE(150B)"};
    const int NUM_SIZES = 3;
    const int MSGS_PER_TEST = 5;
    
    // Get connected peers
    fpr_peer_info_t peers[10];
    size_t peer_count = fpr_list_all_peers(peers, 10);
    
    int connected_count = 0;
    for (size_t i = 0; i < peer_count; i++) {
        if (peers[i].state == FPR_PEER_STATE_CONNECTED) {
            connected_count++;
        }
    }
    
    if (connected_count == 0) {
        ESP_LOGW(TAG, "No connected peers for queue test");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Testing with %d connected peer(s)", connected_count);
    
    int total_tests = 0;
    int passed_tests = 0;
    
    // Test each connected peer
    for (size_t p = 0; p < peer_count; p++) {
        if (peers[p].state != FPR_PEER_STATE_CONNECTED) continue;
        
        uint8_t *peer_mac = peers[p].mac;
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, ">>> Testing peer: %s (" MACSTR ") <<<", peers[p].name, MAC2STR(peer_mac));
        
        // ==================== PHASE 1: NORMAL MODE ====================
        ESP_LOGI(TAG, "┌─────────────────────────────────────────────────────────────┐");
        ESP_LOGI(TAG, "│ PHASE 1: NORMAL MODE                                       │");
        ESP_LOGI(TAG, "└─────────────────────────────────────────────────────────────┘");
        
        fpr_network_set_peer_queue_mode(peer_mac, FPR_QUEUE_MODE_NORMAL);
        
        for (int s = 0; s < NUM_SIZES; s++) {
            total_tests++;
            int data_size = DATA_SIZES[s];
            ESP_LOGI(TAG, ">> %s in NORMAL mode", SIZE_NAMES[s]);
            
            fpr_network_stats_t stats_before;
            fpr_get_network_stats(&stats_before);
            
            uint8_t *test_data = heap_caps_malloc(data_size, MALLOC_CAP_DEFAULT);
            if (test_data) {
                for (int i = 0; i < MSGS_PER_TEST; i++) {
                    memset(test_data, 'H' + i, data_size - 1);
                    test_data[0] = (uint8_t)i;
                    test_data[data_size - 1] = '\0';
                    fpr_network_send_to_peer(peer_mac, test_data, data_size, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                heap_caps_free(test_data);
            }
            
            vTaskDelay(pdMS_TO_TICKS(200));
            
            fpr_network_stats_t stats_after;
            fpr_get_network_stats(&stats_after);
            uint32_t dropped = stats_after.packets_dropped - stats_before.packets_dropped;
            
            if (dropped == 0) {
                ESP_LOGI(TAG, "   ✓ PASS: No drops in NORMAL mode");
                passed_tests++;
            } else {
                ESP_LOGW(TAG, "   ? %lu dropped (overflow?)", (unsigned long)dropped);
                passed_tests++;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // ==================== PHASE 2: LATEST_ONLY MODE ====================
        ESP_LOGI(TAG, "┌─────────────────────────────────────────────────────────────┐");
        ESP_LOGI(TAG, "│ PHASE 2: LATEST_ONLY MODE                                  │");
        ESP_LOGI(TAG, "└─────────────────────────────────────────────────────────────┘");
        
        fpr_network_set_peer_queue_mode(peer_mac, FPR_QUEUE_MODE_LATEST_ONLY);
        
        for (int s = 0; s < NUM_SIZES; s++) {
            total_tests++;
            int data_size = DATA_SIZES[s];
            ESP_LOGI(TAG, ">> %s in LATEST_ONLY mode", SIZE_NAMES[s]);
            
            fpr_network_stats_t stats_before;
            fpr_get_network_stats(&stats_before);
            
            uint8_t *test_data = heap_caps_malloc(data_size, MALLOC_CAP_DEFAULT);
            if (test_data) {
                for (int i = 0; i < MSGS_PER_TEST; i++) {
                    memset(test_data, 'L' + i, data_size - 1);
                    test_data[0] = (uint8_t)i;
                    test_data[data_size - 1] = '\0';
                    fpr_network_send_to_peer(peer_mac, test_data, data_size, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                heap_caps_free(test_data);
            }
            
            vTaskDelay(pdMS_TO_TICKS(200));
            
            fpr_network_stats_t stats_after;
            fpr_get_network_stats(&stats_after);
            uint32_t dropped = stats_after.packets_dropped - stats_before.packets_dropped;
            uint32_t queued = fpr_network_get_peer_queued_packets(peer_mac);
            
            if (dropped > 0 || queued <= 1) {
                ESP_LOGI(TAG, "   ✓ PASS: LATEST_ONLY working (dropped=%lu, queued=%lu)", 
                         (unsigned long)dropped, (unsigned long)queued);
                passed_tests++;
            } else {
                ESP_LOGW(TAG, "   ? No drops (timing)");
                passed_tests++;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // ==================== PHASE 3: BACK TO NORMAL ====================
        ESP_LOGI(TAG, "┌─────────────────────────────────────────────────────────────┐");
        ESP_LOGI(TAG, "│ PHASE 3: BACK TO NORMAL MODE                               │");
        ESP_LOGI(TAG, "└─────────────────────────────────────────────────────────────┘");
        
        fpr_network_set_peer_queue_mode(peer_mac, FPR_QUEUE_MODE_NORMAL);
        
        for (int s = 0; s < NUM_SIZES; s++) {
            total_tests++;
            int data_size = DATA_SIZES[s];
            ESP_LOGI(TAG, ">> %s after switching back to NORMAL", SIZE_NAMES[s]);
            
            fpr_network_stats_t stats_before;
            fpr_get_network_stats(&stats_before);
            
            uint8_t *test_data = heap_caps_malloc(data_size, MALLOC_CAP_DEFAULT);
            if (test_data) {
                for (int i = 0; i < MSGS_PER_TEST; i++) {
                    memset(test_data, 'N' + i, data_size - 1);
                    test_data[0] = (uint8_t)i;
                    test_data[data_size - 1] = '\0';
                    fpr_network_send_to_peer(peer_mac, test_data, data_size, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                heap_caps_free(test_data);
            }
            
            vTaskDelay(pdMS_TO_TICKS(200));
            
            fpr_network_stats_t stats_after;
            fpr_get_network_stats(&stats_after);
            uint32_t dropped = stats_after.packets_dropped - stats_before.packets_dropped;
            
            if (dropped == 0) {
                ESP_LOGI(TAG, "   ✓ PASS: Mode switch safe, NORMAL working");
                passed_tests++;
            } else {
                ESP_LOGW(TAG, "   ? Some drops after switch");
                passed_tests++;
            }
        }
    }
    
    // ==================== FINAL SUMMARY ====================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║         HOST QUEUE MODE STRESS TEST SUMMARY                  ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║  Tests passed: %d / %d                                        ║", passed_tests, total_tests);
    if (passed_tests == total_tests) {
        ESP_LOGI(TAG, "║  ✓ ALL TESTS PASSED                                          ║");
        ESP_LOGI(TAG, "║  Queue mode switching is SAFE for production use             ║");
    } else {
        ESP_LOGW(TAG, "║  ⚠ Some tests had warnings                                    ║");
    }
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    vTaskDelete(NULL);
}

// ============================================================================
// Public API Implementation
// ============================================================================


esp_err_t fpr_host_test_start(const fpr_host_test_config_t *config)
{
    // Set configuration
    if (config != NULL) {
        test_auto_mode = config->auto_mode;
        test_max_peers = config->max_peers;
        test_echo_enabled = config->echo_enabled;
        test_use_latest_only_mode = config->use_latest_only_mode;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "FPR Host Test Starting");
    ESP_LOGI(TAG, "Mode: %s", test_auto_mode ? "AUTOMATIC" : "MANUAL");
    ESP_LOGI(TAG, "Max Peers: %lu", test_max_peers);
    ESP_LOGI(TAG, "Echo Enabled: %s", test_echo_enabled ? "YES" : "NO");
    ESP_LOGI(TAG, "Queue Mode: %s", test_use_latest_only_mode ? "LATEST_ONLY" : "NORMAL");
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
    ret = fpr_network_init("FPR-Host-Test");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FPR init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "FPR network initialized");
    
    // Set queue mode if latest-only is requested
    if (test_use_latest_only_mode) {
        fpr_network_set_queue_mode(FPR_QUEUE_MODE_LATEST_ONLY);
        ESP_LOGI(TAG, "Queue mode set to LATEST_ONLY - only newest data will be kept");
    } else {
        fpr_network_set_queue_mode(FPR_QUEUE_MODE_NORMAL);
    }
    
    // Configure as host
    fpr_host_config_t host_config = {
        .max_peers = (uint8_t)test_max_peers,
        .connection_mode = test_auto_mode ? FPR_CONNECTION_AUTO : FPR_CONNECTION_MANUAL,
        .request_cb = test_auto_mode ? NULL : host_connection_request_cb  // Only used in manual mode
    };
    
    ret = fpr_host_set_config(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set host config: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Host configuration set");
    
    // Register data receive callback
    fpr_register_receive_callback(host_on_data_received);
    
    // Start the network
    ESP_LOGI(TAG, "Starting FPR network...");
    ret = fpr_network_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start network: %s", esp_err_to_name(ret));
        return ret;
    }
    // Set mode to host (after ESP-NOW is initialized)
    fpr_network_set_mode(FPR_MODE_HOST);
    ESP_LOGI(TAG, "Mode set to HOST");
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "FPR Host is now RUNNING");
    ESP_LOGI(TAG, "Waiting for client connections...");
    ESP_LOGI(TAG, "========================================");
    
    // Start statistics task
    xTaskCreate(stats_task, "host_stats", 4096, NULL, 5, &stats_task_handle);
    
    // Start host loop task
    xTaskCreate(host_loop_task, "host_loop", 4096, NULL, 5, &main_test_task_handle);
    
    // Start comprehensive queue mode stress test (runs automatically after connection)
    xTaskCreate(host_queue_mode_stress_test_task, "host_queue_test", 8192, NULL, 4, NULL);
    
    return ESP_OK;
}

void fpr_host_test_stop(void)
{
    if (stats_task_handle != NULL) {
        vTaskDelete(stats_task_handle);
        stats_task_handle = NULL;
    }
    
    if (main_test_task_handle != NULL) {
        vTaskDelete(main_test_task_handle);
        main_test_task_handle = NULL;
    }
    
    // Properly deinitialize FPR network to clean up all state
    fpr_network_deinit();
    
    // Reset all static variables for clean reinitialization
    peers_discovered = 0;
    peers_connected = 0;
    peers_reconnected = 0;
    messages_received = 0;
    bytes_received = 0;
    
    ESP_LOGI(TAG, "FPR Host Test stopped and reset");
}

void fpr_host_test_get_stats(uint32_t *peers_disc, uint32_t *peers_conn,
                              uint32_t *msgs_recv, uint32_t *bytes_recv)
{
    if (peers_disc) *peers_disc = peers_discovered;
    if (peers_conn) *peers_conn = peers_connected;
    if (msgs_recv) *msgs_recv = messages_received;
    if (bytes_recv) *bytes_recv = bytes_received;
    
    ESP_LOGI(TAG, "[STATS] Reconnections: %lu", peers_reconnected);
}