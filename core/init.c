/**
 * @file init.c
 * @brief Sprout Core Lifecycle and Orchestration
 *
 * Core initialization, deinitialization, mode switching, and task management.
 */

#include "sprout/internal/helpers.h"
#include "sprout/sprout.h"
#include "sprout/internal/private_defs.h"
#include "sprout/sprout_config.h"
#include "sprout/sprout_lts.h"
#include "sprout/sprout_legacy.h"
#include "sprout/sprout_handle.h"
#include "sprout/sprout_client.h"
#include "sprout/sprout_extender.h"
#include "sprout/sprout_host.h"
#include <time.h>
#include "hashmap.h"
#include "hashmap_presets.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "string.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "version_control.h"

#define SPROUT_HASHMAP_INITIAL_SIZE 32
#define SPROUT_CONNECT_NAME_SIZE 32
#define SPROUT_NETWORK_VERSION SPROUT_PROTOCOL_VERSION

sprout_network_t sprout_net = {0};

static esp_now_peer_info_t broadcast_info;
static const char *TAG = "sprout";

static void _setup_broadcast_peer(void)
{
    const uint8_t broadcast_mac[6] = SPROUT_BROADCAST_ADDRESS;
    memcpy(broadcast_info.peer_addr, broadcast_mac, 6);
    sprout_set_peer_info(&broadcast_info);
}

static esp_err_t _add_broadcast_peer(const char *mode_name)
{
    esp_now_del_peer(broadcast_info.peer_addr);
    esp_err_t err = esp_now_add_peer(&broadcast_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Broadcast peer added for %s", mode_name);
    }
    return err;
}

static void _handle_default_send_complete(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    #if (SPROUT_DEBUG == 1)
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "Data sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send data: %d", status);
    }
    #endif
}

static void _client_loop_task(void *arg)
{
    TickType_t duration = (TickType_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "Client loop task started for %u ticks", (unsigned int)duration);
    TickType_t start = xTaskGetTickCount();
    TickType_t last_wake = start;

    while ((xTaskGetTickCount() - start) < duration) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SPROUT_CLIENT_WAIT_CHECK_INTERVAL_MS));
    }

    sprout_net.loop_task = NULL;
    vTaskDelete(NULL);
}

static void _host_loop_task(void *arg)
{
    TickType_t duration = (TickType_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "Host loop task started for %u ticks", (unsigned int)duration);
    TickType_t start = xTaskGetTickCount();
    TickType_t last_wake = start;

    while ((xTaskGetTickCount() - start) < duration) {
        sprout_broadcast_device_info();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SPROUT_HOST_SCAN_POLL_INTERVAL_MS));
    }

    sprout_net.loop_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t _sprout_override_protocol(esp_now_send_cb_t sender, esp_now_recv_cb_t receiver)
{
    if (sender) {
        ESP_RETURN_ON_ERROR(esp_now_unregister_send_cb(), TAG, "Failed to unregister send callback");
        ESP_RETURN_ON_ERROR(esp_now_register_send_cb(sender), TAG, "Failed to register send callback");
        sprout_net.sender = sender;
    }

    if (receiver) {
        ESP_RETURN_ON_ERROR(esp_now_unregister_recv_cb(), TAG, "Failed to unregister receive callback");
        ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(receiver), TAG, "Failed to register receive callback");
        sprout_net.receiver = receiver;
    }

    return ESP_OK;
}

esp_err_t sprout_init(const char *name)
{
    ESP_RETURN_ON_FALSE(name != NULL, ESP_ERR_INVALID_ARG, TAG, "Name is NULL");
    ESP_RETURN_ON_FALSE(strlen(name) < sizeof(sprout_net.name), ESP_ERR_INVALID_ARG, TAG, "Name too long");
    ESP_RETURN_ON_FALSE(sprout_net.state == SPROUT_STATE_UNINITIALIZED, ESP_ERR_INVALID_STATE, TAG, "Network already initialized");
    ESP_RETURN_ON_ERROR(esp_read_mac(sprout_net.mac, ESP_MAC_WIFI_STA), TAG, "Failed to read MAC address");

    strncpy(sprout_net.name, name, sizeof(sprout_net.name) - 1);
    sprout_net.name[sizeof(sprout_net.name) - 1] = '\0';
    
    // Set defaults from config
    sprout_net.channel = SPROUT_WIFI_CHANNEL;
    sprout_net.power_mode = (sprout_power_mode_t)SPROUT_DEFAULT_POWER_MODE;
    sprout_net.default_queue_mode = (sprout_queue_mode_t)SPROUT_DEFAULT_QUEUE_MODE;

    if (sprout_net.channel >= 1 && sprout_net.channel <= 14) {
        esp_err_t err = esp_wifi_set_channel(sprout_net.channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi channel %d: %s", sprout_net.channel, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi channel set to %d", sprout_net.channel);
        }
    }

    _setup_broadcast_peer();
    _add_broadcast_peer("default");

    sprout_net.access_state = SPROUT_VISIBILITY_PUBLIC;
    sprout_net.host_config.max_peers = 32;
    sprout_net.host_config.connection_mode = SPROUT_CONNECTION_AUTO;
    sprout_net.host_config.request_cb = NULL;
    sprout_net.client_config.connection_mode = SPROUT_CONNECTION_AUTO;
    sprout_net.client_config.discovery_cb = NULL;
    sprout_net.host_pwk_valid = false;
    sprout_net.tx_sequence_num = 0;

    sprout_net.peers_map = malloc(sizeof(struct hashmap));
    hashmap_init(sprout_net.peers_map, SPROUT_HASHMAP_INITIAL_SIZE, mac_hash, mac_equals);
    
    sprout_net.allowlist_map = NULL;
    sprout_net.allowlist_enabled = false;
    
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "Failed to initialize ESP-NOW");

    sprout_net.state = SPROUT_STATE_INITIALIZED;
    sprout_net.paused = false;

    ESP_LOGI(TAG, "Sprout Network initialized: %s (" MACSTR ") ch=%d pwr=%s queue=%s",
             sprout_net.name, MAC2STR(sprout_net.mac),
             sprout_net.channel ? sprout_net.channel : 0,
             sprout_net.power_mode == SPROUT_POWER_LOW ? "LOW" : "NORMAL",
             sprout_net.default_queue_mode == SPROUT_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
    return ESP_OK;
}

esp_err_t sprout_deinit(void)
{
    if (sprout_net.reconnect_task != NULL) {
        vTaskDelete(sprout_net.reconnect_task);
        sprout_net.reconnect_task = NULL;
    }

    if (sprout_net.loop_task != NULL) {
        vTaskDelete(sprout_net.loop_task);
        sprout_net.loop_task = NULL;
    }

    _reset_all_peers();
    hashmap_free(sprout_net.peers_map);
    free(sprout_net.peers_map);

    if (sprout_net.allowlist_map) {
        hashmap_free(sprout_net.allowlist_map);
        free(sprout_net.allowlist_map);
    }

    esp_err_t esp_now_result = esp_now_deinit();

    memset(&sprout_net, 0, sizeof(sprout_net));
    sprout_net.state = SPROUT_STATE_UNINITIALIZED;

    return esp_now_result;
}

esp_err_t sprout_start()
{
    wifi_mode_t mode;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "WiFi is not initialized");
    ESP_RETURN_ON_FALSE(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA, ESP_ERR_INVALID_STATE, TAG, "WiFi is not started or in STA/APSTA mode");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(_handle_default_send_complete), TAG, "Failed to register send callback");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(_handle_client_discovery), TAG, "Failed to register receive callback");

    sprout_net.sender = _handle_default_send_complete;
    sprout_net.receiver = _handle_client_discovery;
    sprout_set_mode(SPROUT_MODE_CLIENT);

    sprout_net.state = SPROUT_STATE_STARTED;
    sprout_net.paused = false;

    ESP_LOGI(TAG, "Sprout Network started with MAC: " MACSTR, MAC2STR(sprout_net.mac));
    return ESP_OK;
}

esp_err_t sprout_stop()
{
    if (sprout_net.state == SPROUT_STATE_STOPPED || sprout_net.state == SPROUT_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Network already stopped or not initialized");
        return ESP_OK;
    }

    sprout_net.state = SPROUT_STATE_STOPPED;
    sprout_net.paused = false;
    ESP_LOGI(TAG, "Network stopped");
    return ESP_OK;
}

void sprout_set_mode(sprout_mode_type_t mode)
{
    sprout_net.current_mode = mode;

    if (mode == SPROUT_MODE_CLIENT) {
        _add_broadcast_peer("client");
        _sprout_override_protocol(NULL, _handle_client_discovery);
    }
    else if (mode == SPROUT_MODE_HOST) {
        _add_broadcast_peer("host");
        if (sprout_security_generate_pwk(sprout_net.host_pwk) == ESP_OK) {
            sprout_net.host_pwk_valid = true;
            ESP_LOGI(TAG, "Host mode set with generated PWK");
        } else {
            ESP_LOGE(TAG, "Failed to generate PWK for host mode");
        }
        _sprout_override_protocol(NULL, _handle_host_receive);
    }
    else if (mode == SPROUT_MODE_EXTENDER) {
        _add_broadcast_peer("extender");
        _sprout_override_protocol(NULL, _handle_extender_receive);
    }
}

sprout_mode_type_t sprout_get_mode()
{
    return sprout_net.current_mode;
}

esp_err_t sprout_start_loop_task(TickType_t duration, bool force_restart)
{
    if (sprout_net.loop_task != NULL && !force_restart) {
        return ESP_ERR_INVALID_STATE;
    }

    if (force_restart && sprout_net.loop_task != NULL) {
        vTaskDelete(sprout_net.loop_task);
        sprout_net.loop_task = NULL;
    }

    BaseType_t result;
    if (sprout_net.current_mode == SPROUT_MODE_CLIENT) {
        result = xTaskCreate(_client_loop_task, "SPROUT_Client_Loop", 4096, (void *)(uintptr_t)duration, tskIDLE_PRIORITY + 1, &sprout_net.loop_task);
    }
    else if (sprout_net.current_mode == SPROUT_MODE_HOST) {
        result = xTaskCreate(_host_loop_task, "SPROUT_Host_Loop", 4096, (void *)(uintptr_t)duration, tskIDLE_PRIORITY + 1, &sprout_net.loop_task);
    }
    else if (sprout_net.current_mode == SPROUT_MODE_EXTENDER) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    else {
        return ESP_ERR_INVALID_STATE;
    }

    taskYIELD();
    return (result == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sprout_stop_loop_task()
{
    if (sprout_net.loop_task == NULL) {
        vTaskDelete(sprout_net.loop_task);
        sprout_net.loop_task = NULL;
        return ESP_OK;
    }
    else {
        return ESP_ERR_INVALID_STATE;
    }
}

bool sprout_is_loop_task_running()
{
    return (sprout_net.loop_task != NULL);
}

esp_err_t sprout_start_reconnect_task(void)
{
    if (sprout_net.reconnect_task != NULL) {
        ESP_LOGW(TAG, "Reconnect task already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t result;
    if (sprout_net.current_mode == SPROUT_MODE_CLIENT) {
        result = xTaskCreatePinnedToCore(_client_reconnect_task, "SPROUT_Client_Reconnect", SPROUT_TASK_STACK_SIZE, NULL, SPROUT_TASK_PRIORITY, &sprout_net.reconnect_task, SPROUT_RECONNECT_TASK_CORE_PIN_VALUE);
    }
    else if (sprout_net.current_mode == SPROUT_MODE_HOST) {
        result = xTaskCreatePinnedToCore(_host_reconnect_task, "SPROUT_Host_Reconnect", SPROUT_TASK_STACK_SIZE, NULL, SPROUT_TASK_PRIORITY, &sprout_net.reconnect_task, SPROUT_RECONNECT_TASK_CORE_PIN_VALUE);
    }
    else {
        ESP_LOGE(TAG, "Cannot start reconnect task - invalid mode (must be CLIENT or HOST)");
        return ESP_ERR_INVALID_STATE;
    }

    if (result == pdPASS) {
        ESP_LOGI(TAG, "Reconnect task started for %s mode", sprout_net.current_mode == SPROUT_MODE_CLIENT ? "client" : "host");
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t sprout_stop_reconnect_task(void)
{
    if (sprout_net.reconnect_task == NULL) {
        return ESP_OK;
    }

    vTaskDelete(sprout_net.reconnect_task);
    sprout_net.reconnect_task = NULL;

    ESP_LOGI(TAG, "Reconnect task stopped (handlers/state unchanged)");
    return ESP_OK;
}

bool sprout_is_reconnect_task_running(void)
{
    return (sprout_net.reconnect_task != NULL);
}

void sprout_register_receive_callback(sprout_data_receive_cb_t callback)
{
    sprout_net.data_callback = callback;
    if (callback) {
        ESP_LOGI(TAG, "Data receive callback registered");
    } else {
        ESP_LOGI(TAG, "Data receive callback unregistered");
    }
}

// ========== Configuration Setters ==========

void sprout_set_channel(uint8_t channel)
{
    sprout_net.channel = channel;
    if (channel >= 1 && channel <= 14) {
        esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi channel %d: %s", channel, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi channel set to %d", channel);
        }
    }
}

void sprout_set_power_mode(sprout_power_mode_t mode)
{
    sprout_net.power_mode = mode;
    ESP_LOGI(TAG, "Power mode set to %s", mode == SPROUT_POWER_LOW ? "LOW" : "NORMAL");
}

sprout_power_mode_t sprout_get_power_mode(void)
{
    return sprout_net.power_mode;
}

uint8_t sprout_get_channel(void)
{
    return sprout_net.channel;
}

void sprout_set_queue_mode(sprout_queue_mode_t mode)
{
    sprout_net.default_queue_mode = mode;
    ESP_LOGI(TAG, "Queue mode set to %s", mode == SPROUT_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
}

sprout_queue_mode_t sprout_get_queue_mode(void)
{
    return sprout_net.default_queue_mode;
}

void sprout_set_permission_state(sprout_visibility_t state)
{
    sprout_net.access_state = state;
    ESP_LOGI(TAG, "Permission state set to %s", state == SPROUT_VISIBILITY_PRIVATE ? "PRIVATE" : "PUBLIC");
}

sprout_visibility_t sprout_get_permission_state(void)
{
    return sprout_net.access_state;
}

sprout_network_state_t sprout_get_state(void)
{
    return sprout_net.state;
}

esp_err_t sprout_pause(void)
{
    if (sprout_net.state != SPROUT_STATE_STARTED) {
        return ESP_ERR_INVALID_STATE;
    }
    sprout_net.paused = true;
    ESP_LOGI(TAG, "Network paused");
    return ESP_OK;
}

esp_err_t sprout_resume(void)
{
    if (sprout_net.state != SPROUT_STATE_STARTED || !sprout_net.paused) {
        return ESP_ERR_INVALID_STATE;
    }
    sprout_net.paused = false;
    ESP_LOGI(TAG, "Network resumed");
    return ESP_OK;
}

code_version_t sprout_get_protocol_version(void)
{
    return SPROUT_NETWORK_VERSION;
}

void sprout_get_protocol_version_string(char *buf, size_t buf_size)
{
    if (buf && buf_size > 0) {
        snprintf(buf, buf_size, "%" PRId32 ".%" PRId32 ".%" PRId32,
                 (uint32_t)CODE_VERSION_MAJOR(SPROUT_NETWORK_VERSION),
                 (uint32_t)CODE_VERSION_MINOR(SPROUT_NETWORK_VERSION),
                 (uint32_t)CODE_VERSION_PATCH(SPROUT_NETWORK_VERSION));
    }
}
