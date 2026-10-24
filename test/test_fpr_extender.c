/**
 * FPR Extender Test Library
 * 
 * Tests the FPR library in Extender mode.
 * This test acts as a relay node, forwarding messages between peers.
 * 
 * Usage from main.cpp:
 *   #include "test_fpr_extender.h"
 *   
 *   fpr_extender_test_start();
 */

#include "test_fpr_extender.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fpr/fpr.h"
#include "fpr/internal/private_defs.h"
#include "esp_mac.h"

static const char *TAG = "FPR_EXTENDER_TEST";

/* The app owns its node — static storage via fpr_init_at(). */
static fpr_node_t s_node;
static const fpr_init_config_t FPR_INIT_CFG = FPR_INIT_CONFIG_DEFAULT();

static StackType_t s_loop_stack[FPR_TASK_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_loop_tcb;
static const fpr_task_slot_t s_loop_slot = {
    .stack = s_loop_stack, .stack_bytes = sizeof(s_loop_stack),
    .tcb = &s_loop_tcb, .priority = 5, .core = 0,
};
static StackType_t s_recon_stack[FPR_TASK_STACK_SIZE / sizeof(StackType_t)];
static StaticTask_t s_recon_tcb;
static const fpr_task_slot_t s_recon_slot = {
    .stack = s_recon_stack, .stack_bytes = sizeof(s_recon_stack),
    .tcb = &s_recon_tcb, .priority = 5, .core = 0,
};

// Statistics
static uint32_t messages_relayed = 0;
static uint32_t bytes_relayed = 0;

// Task handles
static TaskHandle_t stats_task_handle = NULL;
static TaskHandle_t monitor_task_handle = NULL;

/**
 * Note: Extender mode operates automatically without callbacks.
 * The FPR library handles message relaying internally.
 * Statistics are retrieved via fpr_get_stats(&s_node).
 */

/**
 * Statistics task
 */
static void stats_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10000)); // Every 10 seconds
        
        fpr_stats_t stats;
        fpr_get_stats(&s_node, &stats);
        
        ESP_LOGI(TAG, "========== STATISTICS ==========");
        ESP_LOGI(TAG, "Packets sent: %lu", (unsigned long)stats.packets_sent);
        ESP_LOGI(TAG, "Packets received: %lu", (unsigned long)stats.packets_received);
        ESP_LOGI(TAG, "Packets forwarded: %lu", (unsigned long)stats.packets_forwarded);
        ESP_LOGI(TAG, "Packets dropped: %lu", (unsigned long)stats.packets_dropped);
        ESP_LOGI(TAG, "Send failures: %lu", (unsigned long)stats.send_failures);
        ESP_LOGI(TAG, "Replay attacks blocked: %lu", (unsigned long)stats.replay_attacks_blocked);
        ESP_LOGI(TAG, "Known peers: %zu", stats.peer_count);
        
        // Show queue depths for all known peers
        fpr_peer_info_t peers[10];
        size_t count = ({size_t _v=0;(void)fpr_list_peers(&s_node, peers, 10,&_v);_v;});
        if (count > 0) {
            ESP_LOGI(TAG, "=== Peer Queue Status ===");
            for (size_t i = 0; i < count; i++) {
                uint32_t q = ({uint32_t _v=0;(void)fpr_peer_queued_packets(&s_node, peers[i].mac,&_v);_v;});
                ESP_LOGI(TAG, "  %s (" MACSTR "): %lu queued, state=%d",
                         peers[i].name, MAC2STR(peers[i].mac), (unsigned long)q, peers[i].state);
            }
        }
        ESP_LOGI(TAG, "================================");
        
        // Update local counters for get_stats API
        messages_relayed = stats.packets_forwarded;
        bytes_relayed = 0;  // Byte count not available in network stats
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
 * Monitor task
 */
static void monitor_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Every minute
        
        fpr_stats_t stats;
        fpr_get_stats(&s_node, &stats);
        
        ESP_LOGI(TAG, "[MONITOR] Extender running, %lu messages forwarded", 
                 (unsigned long)stats.packets_forwarded);
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

esp_err_t fpr_extender_test_start(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "FPR Extender Test Starting");
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
    ret = fpr_init_at(&s_node, "FPR-Extender-Test", &FPR_INIT_CFG);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FPR init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "FPR network initialized");
    
    // Set mode to extender (no configuration needed)
    fpr_set_mode(&s_node, FPR_MODE_EXTENDER);
    ESP_LOGI(TAG, "Mode set to EXTENDER");
    
    // Start the network
    ESP_LOGI(TAG, "Starting FPR network...");
    ret = fpr_start(&s_node);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start network: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "FPR Extender is now RUNNING");
    ESP_LOGI(TAG, "Ready to relay messages...");
    ESP_LOGI(TAG, "========================================");
    
    // Start tasks
    xTaskCreate(stats_task, "ext_stats", 4096, NULL, 5, &stats_task_handle);
    xTaskCreate(monitor_task, "ext_mon", 4096, NULL, 5, &monitor_task_handle);
    
    return ESP_OK;
}

void fpr_extender_test_stop(void)
{
    if (stats_task_handle != NULL) {
        vTaskDelete(stats_task_handle);
        stats_task_handle = NULL;
    }
    
    if (monitor_task_handle != NULL) {
        vTaskDelete(monitor_task_handle);
        monitor_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "FPR Extender Test stopped");
}

void fpr_extender_test_get_stats(uint32_t *msgs_relayed, uint32_t *bytes_rel)
{
    if (msgs_relayed) *msgs_relayed = messages_relayed;
    if (bytes_rel) *bytes_rel = bytes_relayed;
}
