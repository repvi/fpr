/**
 * @file fpr.c
 * @brief FPR (Fast Peer Router) Core Implementation
 *
 * This is the main implementation file for the FPR protocol, providing
 * peer discovery, connection management, and data routing functionality
 * over ESP-NOW for ESP32 family devices.
 *
 * Core responsibilities:
 * - Network initialization and deinitialization
 * - ESP-NOW callback registration and mode dispatch
 * - Loop/reconnect task control
 * - State, power, channel, and queue-mode accessors
 *
 * Peer management lives in fpr_peers.c, the send path in fpr_send.c,
 * and statistics/diagnostics in fpr_diag.c.
 *
 * Thread Safety:
 * - Most operations are thread-safe via FreeRTOS primitives
 * - Callbacks may be invoked from ESP-NOW context (ISR-like)
 * - Application callbacks should be non-blocking
 *
 * Memory Management:
 * - Uses heap allocation for peer structures
 * - Queue-based message buffering
 * - Configurable queue depths via Kconfig
 *
 * @author Alejandro Ramirez
 * @version 1.0.0
 * @date December 2025
 * @since August 2025
 */

#include "fpr/fpr.h"
#include "fpr/internal/helpers.h"
#include "fpr/internal/rx.h"
#include "fpr/fpr_config.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_check.h"

// Protocol configuration
#define FPR_HASHMAP_INITIAL_SIZE 32
/* Drain window in fpr_network_deinit for ESP-NOW RX callbacks that
 * were already dispatched when the recv cb was unregistered — they
 * run on the wifi task, which outranks every deinit caller, so this
 * only has to cover a few scheduling opportunities. */
#define FPR_DEINIT_SETTLE_MS 50

fpr_network_t fpr_net = {0}; // extern from private_defs.h

static esp_now_peer_info_t broadcast_info;

static const char *TAG = "fpr";

/* Cooperative-stop grace for the loop/reconnect tasks: they poll their
 * stop flag once per configured poll interval (power-mode stretched),
 * so the window must cover one worst-case iteration plus lock hold. */
static inline TickType_t _task_stop_grace(void)
{
    uint32_t grace_ms =
        2 * _power_interval(FPR_HOST_SCAN_POLL_INTERVAL_MS) + 250;
    return pdMS_TO_TICKS(grace_ms);
}

/* Request a cooperative stop and join on the handle clearing its own
 * slot. vTaskDelete remains the fallback for a wedged task — but only
 * after a full grace window, because a task killed while holding
 * peers_lock strands the recursive mutex for every later caller. */
void _task_stop(TaskHandle_t *handle, volatile bool *stop_flag)
{
    if (handle == NULL || *handle == NULL) {
        return;
    }
    TaskHandle_t target = *handle;
    *stop_flag = true;
    TickType_t deadline = xTaskGetTickCount() + _task_stop_grace();
    while (*handle == target &&
           (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (*handle == target) {
        ESP_LOGW(TAG, "FPR task did not exit within grace — deleting");
        vTaskDelete(target);
        *handle = NULL;
    }
    *stop_flag = false;
}

/* Exit path for an fpr-owned task: clear the handle slot only if it
 * still names this task — a restarted instance may already have
 * replaced it — then self-delete. */
void _task_exit(TaskHandle_t *slot)
{
    _peers_lock();
    if (*slot == xTaskGetCurrentTaskHandle()) {
        *slot = NULL;
    }
    _peers_unlock();
    vTaskDelete(NULL);
}

// Helper: Setup broadcast peer configuration

static void _broadcast_peer_setup(void)
{
    const uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    memcpy(broadcast_info.peer_addr, broadcast_mac, 6);
    fpr_set_peer_info(&broadcast_info);
}

// Helper: Add broadcast peer to ESP-NOW
// this needs to be called whenever we change mode (host/client/extender)
static esp_err_t _broadcast_peer_add(const char *mode_name)
{
    esp_now_del_peer(broadcast_info.peer_addr);
    esp_err_t err = esp_now_add_peer(&broadcast_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Broadcast peer added for %s", mode_name);
    }
    return err;
}

esp_err_t fpr_network_init(const char *name)
{
    // Use Kconfig defaults
    fpr_init_config_t config = {
        .channel = FPR_WIFI_CHANNEL,
        .power_mode = (fpr_power_mode_t)FPR_DEFAULT_POWER_MODE,
        .queue_mode = (fpr_queue_mode_t)FPR_DEFAULT_QUEUE_MODE
    };
    return fpr_network_init_ex(name, &config);
}

esp_err_t fpr_network_init_ex(const char *name, const fpr_init_config_t *config)
{
    ESP_RETURN_ON_FALSE(name != NULL, ESP_ERR_INVALID_ARG, TAG, "Name is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    ESP_RETURN_ON_FALSE(strlen(name) < sizeof(fpr_net.name), ESP_ERR_INVALID_ARG, TAG, "Name too long");
    ESP_RETURN_ON_FALSE(fpr_net.state == FPR_STATE_UNINITIALIZED, ESP_ERR_INVALID_STATE, TAG, "Network already initialized");
    ESP_RETURN_ON_ERROR(esp_read_mac(fpr_net.mac, ESP_MAC_WIFI_STA), TAG, "Failed to read MAC address");
    
    strncpy(fpr_net.name, name, sizeof(fpr_net.name) - 1);
    fpr_net.name[sizeof(fpr_net.name) - 1] = '\0';
    // Store channel and power mode config
    fpr_net.channel = config->channel;
    fpr_net.power_mode = config->power_mode;
    fpr_net.default_queue_mode = config->queue_mode;
    
    // Set WiFi channel if specified (must be done before ESP-NOW init)
    if (config->channel >= 1 && config->channel <= 14) {
        esp_err_t err = esp_wifi_set_channel(config->channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi channel %d: %s", config->channel, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi channel set to %d", config->channel);
        }
    }
    
    // Initialize broadcast peer info
    _broadcast_peer_setup();
    _broadcast_peer_add("default");
    
    fpr_net.access_state = FPR_VISIBILITY_PUBLIC;
    
    // Initialize default host config
    fpr_net.host_config.max_peers = 32;  // 32 by default
    fpr_net.host_config.connection_mode = FPR_CONNECTION_AUTO;
    fpr_net.host_config.request_cb = NULL;
    
    // Initialize default client config
    fpr_net.client_config.connection_mode = FPR_CONNECTION_AUTO;
    fpr_net.client_config.discovery_cb = NULL;

    // Initialize security
    fpr_net.host_pwk_valid = false;
    
    // Initialize sequence number counter
    fpr_net.tx_sequence_num = 0;

    /* peers_lock is process-lifetime: created lazily here and never
     * deleted. A callback or reader may legitimately reach for it after
     * deinit (esp_now callbacks already dispatched, a blocked
     * get_data_from_peer waking late) — deleting the semaphore under a
     * waiter is UB, and the guarded objects are left in hashmap's
     * safe-empty state anyway. */
    if (fpr_net.peers_lock == NULL) {
        fpr_net.peers_lock = xSemaphoreCreateRecursiveMutex();
        if (fpr_net.peers_lock == NULL) {
            ESP_LOGE(TAG, "Failed to allocate peers lock");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!hashmap_init(&fpr_net.peers_map, FPR_HASHMAP_INITIAL_SIZE, mac_hash, mac_equals)) {
        ESP_LOGE(TAG, "Failed to allocate peers map");
        return ESP_ERR_NO_MEM;
    }
    if (!hashmap_init(&fpr_net.routes_map, FPR_HASHMAP_INITIAL_SIZE, mac_hash, mac_equals)) {
        ESP_LOGE(TAG, "Failed to allocate routes map");
        hashmap_free(&fpr_net.peers_map);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t esp_now_err = esp_now_init();
    if (esp_now_err != ESP_OK) {
        /* The maps are already allocated — free them or every init retry
         * on a degraded radio leaks the bucket arrays. */
        hashmap_free(&fpr_net.routes_map);
        hashmap_free(&fpr_net.peers_map);
        ESP_LOGE(TAG, "Failed to initialize ESP-NOW: %s", esp_err_to_name(esp_now_err));
        return esp_now_err;
    }
    
    fpr_net.state = FPR_STATE_INITIALIZED;
    fpr_net.paused = false;
    
    ESP_LOGI(TAG, "FPR Network initialized: %s (" MACSTR ") ch=%d pwr=%s queue=%s", 
             fpr_net.name, MAC2STR(fpr_net.mac),
             fpr_net.channel ? fpr_net.channel : 0,
             fpr_net.power_mode == FPR_POWER_LOW ? "LOW" : "NORMAL",
             fpr_net.default_queue_mode == FPR_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
    return ESP_OK;
}

esp_err_t fpr_network_deinit(void)
{
    /* Quiesce gate FIRST — peer adds and extender route learning reject
     * immediately; a callback still in flight sees UNINITIALIZED when it
     * re-checks state under the lock, before any teardown below runs. */
    _peers_lock();
    fpr_net.state = FPR_STATE_UNINITIALIZED;
    _peers_unlock();

    /* Drop the recv callback before tearing down: nothing new is
     * dispatched on the wifi task after this point, so only callbacks
     * already in flight remain below. */
    esp_now_unregister_recv_cb();

    /* Ask the internal tasks to exit rather than vTaskDelete them: a
     * task killed while holding peers_lock strands the recursive mutex
     * for every later caller (FreeRTOS never releases a dead task's
     * mutexes). The helper joins on the task clearing its own handle
     * slot inside a poll-interval grace window. */
    _task_stop(&fpr_net.reconnect_task, &fpr_net.reconnect_stop_requested);
    _task_stop(&fpr_net.loop_task, &fpr_net.loop_stop_requested);

    esp_err_t esp_now_result = esp_now_deinit();

    /* Drain the callbacks dispatched before the unregister: they finish
     * on the still-valid lock + maps (writes gated by the state check;
     * reads still see valid entries). The wifi task outranks every
     * deinit caller, so a modest delay guarantees it got scheduled. */
    vTaskDelay(pdMS_TO_TICKS(FPR_DEINIT_SETTLE_MS));

    /* Now free under the lock. A pathological straggler that still
     * wakes late re-locks a LIVE mutex and reads maps that
     * hashmap_free leaves in a safe empty state — every map op is a
     * no-op returning NULL on NULL buckets — and each deinit pass
     * restores exactly that state for the next cycle. */
    _peers_lock();
    _reset_all_peers();
    hashmap_free(&fpr_net.peers_map);
    hashmap_foreach(&fpr_net.routes_map, _route_free_cb, NULL);
    hashmap_free(&fpr_net.routes_map);
    _peers_unlock();

    /* peers_lock survives: a straggler callback or a blocked
     * get_data_from_peer reader may still reach for it after the memset
     * — deleting the semaphore under a waiter would be use-after-free.
     * It is created lazily in init and owned for the process lifetime. */
    SemaphoreHandle_t kept_lock = fpr_net.peers_lock;
    memset(&fpr_net, 0, sizeof(fpr_net));
    fpr_net.state = FPR_STATE_UNINITIALIZED;
    fpr_net.peers_lock = kept_lock;

    return esp_now_result;
}

// default
static void _tx_done(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    #if (FPR_DEBUG == 1)
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "Data sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send data: %d", status);
    }
    #endif
}

esp_err_t fpr_network_start(void)
{
    ESP_RETURN_ON_FALSE(fpr_net.state == FPR_STATE_INITIALIZED || fpr_net.state == FPR_STATE_STOPPED,
                        ESP_ERR_INVALID_STATE, TAG, "Network not initialized (state %d)", fpr_net.state);
    wifi_mode_t mode;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "WiFi is not initialized");
    ESP_RETURN_ON_FALSE(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA, ESP_ERR_INVALID_STATE, TAG, "WiFi is not started or in STA/APSTA mode");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(_tx_done), TAG, "Failed to register send callback");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(_rx_client), TAG, "Failed to register receive callback");

    fpr_network_set_mode(FPR_MODE_CLIENT);

    _peers_lock();
    fpr_net.state = FPR_STATE_STARTED;
    fpr_net.paused = false;
    _peers_unlock();

    ESP_LOGI(TAG, "FPR Network started with MAC: " MACSTR, MAC2STR(fpr_net.mac));
    return ESP_OK;
}

esp_err_t fpr_network_stop(void)
{
    if (fpr_net.state == FPR_STATE_STOPPED || fpr_net.state == FPR_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Network already stopped or not initialized");
        return ESP_OK;
    }
    
    _peers_lock();
    fpr_net.state = FPR_STATE_STOPPED;
    fpr_net.paused = false;
    _peers_unlock();
    ESP_LOGI(TAG, "Network stopped");
    return ESP_OK;
}

static esp_err_t _set_protocol_cbs(esp_now_send_cb_t sender, esp_now_recv_cb_t receiver)
{
    if (sender) {
        ESP_RETURN_ON_ERROR(esp_now_unregister_send_cb(), TAG, "Failed to unregister send callback");
        ESP_RETURN_ON_ERROR(esp_now_register_send_cb(sender), TAG, "Failed to register send callback");
    }

    if (receiver) {
        ESP_RETURN_ON_ERROR(esp_now_unregister_recv_cb(), TAG, "Failed to unregister receive callback");
        ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(receiver), TAG, "Failed to register receive callback");
    }
    
    return ESP_OK;
}

static void _task_client_loop(void *arg)
{
    TickType_t duration = (TickType_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "Client loop task started for %u ticks", (unsigned int)duration);
    TickType_t start = xTaskGetTickCount();
    TickType_t last_wake = start;

    while (!fpr_net.loop_stop_requested &&
           (xTaskGetTickCount() - start) < duration) {
        // Just wait - receiver handler does all the work
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FPR_CLIENT_WAIT_CHECK_INTERVAL_MS));
    }

    _task_exit(&fpr_net.loop_task);
}

static void _task_host_loop(void *arg)
{
    TickType_t duration = (TickType_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "Host loop task started for %u ticks", (unsigned int)duration);
    TickType_t start = xTaskGetTickCount();
    TickType_t last_wake = start;

    while (!fpr_net.loop_stop_requested &&
           (xTaskGetTickCount() - start) < duration) {
        // Just broadcast device info - receiver handler does connection work
        fpr_network_broadcast_device_info();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FPR_HOST_SCAN_POLL_INTERVAL_MS));
    }

    _task_exit(&fpr_net.loop_task);
}

void fpr_network_set_mode(fpr_mode_type_t mode)
{
    fpr_net.current_mode = mode;
    /* Mesh forwarding only runs in extender mode. */
    fpr_net.routing_enabled = (mode == FPR_MODE_EXTENDER);

    // Add broadcast peer for host and client modes (required for broadcasts)
    if (mode == FPR_MODE_CLIENT) {
        _broadcast_peer_add("client");
        _set_protocol_cbs(NULL, _rx_client);
    }
    else if (mode == FPR_MODE_HOST) {
        _broadcast_peer_add("host");
        // Generate host PWK for secure connections — under the lock so
        // rx callbacks never observe a valid flag ahead of the key bytes
        _peers_lock();
        if (fpr_security_generate_pwk(fpr_net.host_pwk) == ESP_OK) {
            fpr_net.host_pwk_valid = true;
            ESP_LOGI(TAG, "Host mode set with generated PWK");
        } else {
            ESP_LOGE(TAG, "Failed to generate PWK for host mode");
        }
        _peers_unlock();
        _set_protocol_cbs(NULL, _rx_host);
    }
    else if (mode == FPR_MODE_EXTENDER) {
        _broadcast_peer_add("extender");
        _set_protocol_cbs(NULL, _rx_extender);
    }
}

fpr_mode_type_t fpr_network_get_mode(void)
{
    return fpr_net.current_mode;
}

esp_err_t fpr_network_start_loop_task(TickType_t duration, bool force_restart)
{
    if (fpr_net.state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE; // Deinit in progress or never inited
    }
    if (fpr_net.loop_task != NULL && !force_restart) {
        return ESP_ERR_INVALID_STATE; // Loop already running
    }

    if (force_restart && fpr_net.loop_task != NULL) {
        _task_stop(&fpr_net.loop_task, &fpr_net.loop_stop_requested);
    }
    fpr_net.loop_stop_requested = false;

    BaseType_t result;
    if (fpr_net.current_mode == FPR_MODE_CLIENT) {
        result = xTaskCreate(_task_client_loop, "FPR_Client_Loop", 4096, (void *)(uintptr_t)duration, tskIDLE_PRIORITY + 1, &fpr_net.loop_task);
    }
    else if (fpr_net.current_mode == FPR_MODE_HOST) {
        result = xTaskCreate(_task_host_loop, "FPR_Host_Loop", 4096, (void *)(uintptr_t)duration, tskIDLE_PRIORITY + 1, &fpr_net.loop_task);
    }
    else if (fpr_net.current_mode == FPR_MODE_EXTENDER) {
        return ESP_ERR_NOT_SUPPORTED; // Extender mode does not support loop tasks
    }
    else {
        return ESP_ERR_INVALID_STATE; // Invalid mode
    }

    taskYIELD(); // Yield to allow task to start
    return (result == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fpr_network_stop_loop_task(void)
{
    if (fpr_net.loop_task == NULL) {
        return ESP_ERR_INVALID_STATE; // No loop task running
    }
    /* Cooperative stop — vTaskDelete here could kill the task while it
     * holds peers_lock, stranding the mutex. */
    _task_stop(&fpr_net.loop_task, &fpr_net.loop_stop_requested);
    return ESP_OK;
}

bool fpr_network_is_loop_task_running(void)
{
    return (fpr_net.loop_task != NULL);
}

// ========== APPLICATION CALLBACK REGISTRATION ==========

void fpr_register_receive_callback(fpr_data_receive_cb_t callback)
{
    fpr_net.data_callback = callback;
    if (callback) {
        ESP_LOGI(TAG, "Data receive callback registered");
    } else {
        ESP_LOGI(TAG, "Data receive callback unregistered");
    }
}

// ========== RECONNECT TASK CONTROL API ==========

esp_err_t fpr_network_start_reconnect_task(void)
{
    if (fpr_net.state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE; // Deinit in progress or never inited
    }
    if (fpr_net.reconnect_task != NULL) {
        ESP_LOGW(TAG, "Reconnect task already running");
        return ESP_ERR_INVALID_STATE;
    }
    fpr_net.reconnect_stop_requested = false;

    BaseType_t result;
    if (fpr_net.current_mode == FPR_MODE_CLIENT) {
        // Override protocol handler to keep connection logic active
        result = xTaskCreatePinnedToCore(_task_client_reconnect, "FPR_Client_Reconnect", FPR_TASK_STACK_SIZE, NULL, FPR_TASK_PRIORITY, &fpr_net.reconnect_task, FPR_RECONNECT_TASK_CORE_PIN_VALUE);
    }
    else if (fpr_net.current_mode == FPR_MODE_HOST) {
        // Override protocol handler to keep connection logic active
        result = xTaskCreatePinnedToCore(_task_host_reconnect, "FPR_Host_Reconnect", FPR_TASK_STACK_SIZE, NULL, FPR_TASK_PRIORITY, &fpr_net.reconnect_task, FPR_RECONNECT_TASK_CORE_PIN_VALUE);
    }
    else {
        ESP_LOGE(TAG, "Cannot start reconnect task - invalid mode (must be CLIENT or HOST)");
        return ESP_ERR_INVALID_STATE;
    }

    if (result == pdPASS) {
        ESP_LOGI(TAG, "Reconnect task started for %s mode", fpr_net.current_mode == FPR_MODE_CLIENT ? "client" : "host");
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t fpr_network_stop_reconnect_task(void)
{
    // Intentionally only stops the background reconnect/keepalive task.
    // Do NOT change protocol handlers or other network state here — stopping
    // reconnect must only remove the automatic reconnection ability.
    if (fpr_net.reconnect_task == NULL) {
        return ESP_OK; // Already stopped
    }

    /* Cooperative stop — vTaskDelete here could kill the task while it
     * holds peers_lock (it sends keepalives inside the critical section),
     * stranding the mutex for every later caller. */
    _task_stop(&fpr_net.reconnect_task, &fpr_net.reconnect_stop_requested);

    ESP_LOGI(TAG, "Reconnect task stopped (handlers/state unchanged)");
    return ESP_OK;
}

bool fpr_network_is_reconnect_task_running(void)
{
    return (fpr_net.reconnect_task != NULL);
}

// ========== NETWORK STATE MANAGEMENT ==========

esp_err_t fpr_network_pause(void)
{
    if (fpr_net.state != FPR_STATE_STARTED) {
        ESP_LOGW(TAG, "Network not started, cannot pause");
        return ESP_ERR_INVALID_STATE;
    }
    
    _peers_lock();
    fpr_net.paused = true;
    fpr_net.state = FPR_STATE_PAUSED;
    _peers_unlock();
    ESP_LOGI(TAG, "Network paused");
    return ESP_OK;
}

esp_err_t fpr_network_resume(void)
{
    if (fpr_net.state != FPR_STATE_PAUSED) {
        ESP_LOGW(TAG, "Network not paused, cannot resume");
        return ESP_ERR_INVALID_STATE;
    }
    
    _peers_lock();
    fpr_net.paused = false;
    fpr_net.state = FPR_STATE_STARTED;
    _peers_unlock();
    ESP_LOGI(TAG, "Network resumed");
    return ESP_OK;
}

fpr_network_state_t fpr_network_get_state(void)
{
    return fpr_net.state;
}

void fpr_network_set_power_mode(fpr_power_mode_t mode)
{
    fpr_net.power_mode = mode;
    ESP_LOGI(TAG, "Power mode set to %s", mode == FPR_POWER_LOW ? "LOW" : "NORMAL");
}

fpr_power_mode_t fpr_network_get_power_mode(void)
{
    return fpr_net.power_mode;
}

uint8_t fpr_network_get_channel(void)
{
    return fpr_net.channel;
}

// ========== QUEUE MODE API ==========

void fpr_network_set_queue_mode(fpr_queue_mode_t mode)
{
    fpr_net.default_queue_mode = mode;
    ESP_LOGI(TAG, "Default queue mode set to %s", 
             mode == FPR_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
}

fpr_queue_mode_t fpr_network_get_queue_mode(void)
{
    return fpr_net.default_queue_mode;
}

