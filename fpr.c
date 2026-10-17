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
 * - ESP-NOW callback registration and packet routing
 * - Peer management (add, remove, lookup)
 * - Send/receive operations with fragmentation support
 * - Network statistics tracking
 * - Power management modes
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

#include "fpr/internal/helpers.h"
#include "fpr/fpr.h"
#include "fpr/internal/private_defs.h"
#include "fpr/fpr_config.h"
#include "fpr/fpr_lts.h"
#include "fpr/fpr_legacy.h" // for legacy packet handling
#include "fpr/fpr_handle.h"  // for version handling
#include "fpr/fpr_client.h"
#include "fpr/fpr_extender.h"
#include "fpr/fpr_host.h"
#include "standard/time.h"
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
#include "lib/version_control.h"

// Protocol configuration
#define FPR_HASHMAP_INITIAL_SIZE 32
#define FPR_CONNECT_NAME_SIZE 32
/* Drain window in fpr_network_deinit for ESP-NOW RX callbacks that
 * were already dispatched when the recv cb was unregistered — they
 * run on the wifi task, which outranks every deinit caller, so this
 * only has to cover a few scheduling opportunities. */
#define FPR_DEINIT_SETTLE_MS 50

// Use version definitions from fpr_lts.h
#define FPR_NETWORK_VERSION FPR_PROTOCOL_VERSION

fpr_network_t fpr_net = {0}; // extern from private_defs.h

static esp_now_peer_info_t broadcast_info;

static const char *TAG = "fpr";

/* Cooperative-stop grace for the loop/reconnect tasks: they poll their
 * stop flag once per configured poll interval (power-mode stretched),
 * so the window must cover one worst-case iteration plus lock hold. */
static inline TickType_t _fpr_task_stop_grace_ticks(void)
{
    uint32_t grace_ms =
        2 * _fpr_get_power_adjusted_interval(FPR_HOST_SCAN_POLL_INTERVAL_MS) + 250;
    return pdMS_TO_TICKS(grace_ms);
}

/* Request a cooperative stop and join on the handle clearing its own
 * slot. vTaskDelete remains the fallback for a wedged task — but only
 * after a full grace window, because a task killed while holding
 * peers_lock strands the recursive mutex for every later caller. */
void _fpr_stop_task(TaskHandle_t *handle, volatile bool *stop_flag)
{
    if (handle == NULL || *handle == NULL) {
        return;
    }
    TaskHandle_t target = *handle;
    *stop_flag = true;
    TickType_t deadline = xTaskGetTickCount() + _fpr_task_stop_grace_ticks();
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
void _fpr_task_exit(TaskHandle_t *slot)
{
    _peers_lock();
    if (*slot == xTaskGetCurrentTaskHandle()) {
        *slot = NULL;
    }
    _peers_unlock();
    vTaskDelete(NULL);
}

/* queued_packets is written by the rx path under peers_lock; the
 * blocking reader here holds only a store reference, so the decrement
 * must take the lock too or the counter desyncs from the real queue. */
static void _peer_dec_queued(FPR_STORE_HASH_TYPE *peer)
{
    _peers_lock();
    if (peer->queued_packets > 0) {
        peer->queued_packets--;
    }
    _peers_unlock();
}

// ========== INTERNAL FUNCTIONS ==========

static esp_err_t _remove_peer_internal(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    _peers_lock();
    /* The map key may point inside the store we are about to release. */
    uint8_t mac[MAC_ADDRESS_LENGTH];
    memcpy(mac, peer_mac, sizeof(mac));
    FPR_STORE_HASH_TYPE *var = _get_peer_from_map(mac);
    esp_err_t result;
    if (var != NULL) {
        if (!hashmap_remove(&fpr_net.peers_map, mac)) {
            _peers_unlock();
            return ESP_FAIL;
        }
        _peer_store_free_or_defer(var);
    }
    result = esp_now_del_peer(mac);
    _peers_unlock();
    return result;
}

static void _cleanup_peer_entry(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    if (peer) {
        esp_now_del_peer(peer->peer_info.peer_addr);
        _peer_store_free_or_defer(peer);
    }
}

static void _reset_all_peers(void)
{
    hashmap_foreach(&fpr_net.peers_map, _cleanup_peer_entry, NULL);
}

static void _free_route_entry(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    heap_caps_free(value);
}

// Helper: Setup broadcast peer configuration
static void _setup_broadcast_peer(void)
{
    const uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    memcpy(broadcast_info.peer_addr, broadcast_mac, 6);
    fpr_set_peer_info(&broadcast_info);
}

// Helper: Add broadcast peer to ESP-NOW
// this needs to be called whenever we change mode (host/client/extender)
static esp_err_t _add_broadcast_peer(const char *mode_name)
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
    _setup_broadcast_peer();
    _add_broadcast_peer("default");
    
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
    _fpr_stop_task(&fpr_net.reconnect_task, &fpr_net.reconnect_stop_requested);
    _fpr_stop_task(&fpr_net.loop_task, &fpr_net.loop_stop_requested);

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
    hashmap_foreach(&fpr_net.routes_map, _free_route_entry, NULL);
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
static void _handle_default_send_complete(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    #if (FPR_DEBUG == 1)
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "Data sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send data: %d", status);
    }
    #endif
}

esp_err_t fpr_network_start()
{
    wifi_mode_t mode;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "WiFi is not initialized");
    ESP_RETURN_ON_FALSE(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA, ESP_ERR_INVALID_STATE, TAG, "WiFi is not started or in STA/APSTA mode");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(_handle_default_send_complete), TAG, "Failed to register send callback");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(_handle_client_discovery), TAG, "Failed to register receive callback");
    
    fpr_net.sender = _handle_default_send_complete;
    fpr_net.receiver = _handle_client_discovery;
    fpr_network_set_mode(FPR_MODE_CLIENT);

    _peers_lock();
    fpr_net.state = FPR_STATE_STARTED;
    fpr_net.paused = false;
    _peers_unlock();

    ESP_LOGI(TAG, "FPR Network started with MAC: " MACSTR, MAC2STR(fpr_net.mac));
    return ESP_OK;
}

esp_err_t fpr_network_stop()
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

static esp_err_t fpr_network_override_protocol(esp_now_send_cb_t sender, esp_now_recv_cb_t receiver)
{
    if (sender) {
        ESP_RETURN_ON_ERROR(esp_now_unregister_send_cb(), TAG, "Failed to unregister send callback");
        ESP_RETURN_ON_ERROR(esp_now_register_send_cb(sender), TAG, "Failed to register send callback");
        fpr_net.sender = sender;
    }
    
    if (receiver) {
        ESP_RETURN_ON_ERROR(esp_now_unregister_recv_cb(), TAG, "Failed to unregister receive callback");
        ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(receiver), TAG, "Failed to register receive callback");
        fpr_net.receiver = receiver;
    }
    
    return ESP_OK;
}

static void _fpr_handle_client_loop(void *arg)
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

    _fpr_task_exit(&fpr_net.loop_task);
}

static void _fpr_handle_host_loop(void *arg)
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

    _fpr_task_exit(&fpr_net.loop_task);
}

void fpr_network_set_mode(fpr_mode_type_t mode)
{
    fpr_net.current_mode = mode;
    
    // Add broadcast peer for host and client modes (required for broadcasts)
    if (mode == FPR_MODE_CLIENT) {
        _add_broadcast_peer("client");
        fpr_network_override_protocol(NULL, _handle_client_discovery);
    }
    else if (mode == FPR_MODE_HOST) {
        _add_broadcast_peer("host");
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
        fpr_network_override_protocol(NULL, _handle_host_receive);
    }
    else if (mode == FPR_MODE_EXTENDER) {
        _add_broadcast_peer("extender");
        fpr_network_override_protocol(NULL, _handle_extender_receive);
    }
}

fpr_mode_type_t fpr_network_get_mode()
{
    return fpr_net.current_mode;
}

esp_err_t fpr_network_add_peer(uint8_t *peer_mac)
{
    return _add_peer_internal(peer_mac, NULL, false, 0);
}

esp_err_t fpr_network_remove_peer(uint8_t *peer_mac)
{
    return _remove_peer_internal(peer_mac);
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
        _fpr_stop_task(&fpr_net.loop_task, &fpr_net.loop_stop_requested);
    }
    fpr_net.loop_stop_requested = false;

    BaseType_t result;
    if (fpr_net.current_mode == FPR_MODE_CLIENT) {
        result = xTaskCreate(_fpr_handle_client_loop, "FPR_Client_Loop", 4096, (void *)(uintptr_t)duration, tskIDLE_PRIORITY + 1, &fpr_net.loop_task);
    }
    else if (fpr_net.current_mode == FPR_MODE_HOST) {
        result = xTaskCreate(_fpr_handle_host_loop, "FPR_Host_Loop", 4096, (void *)(uintptr_t)duration, tskIDLE_PRIORITY + 1, &fpr_net.loop_task);
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

esp_err_t fpr_network_stop_loop_task()
{
    if (fpr_net.loop_task == NULL) {
        return ESP_ERR_INVALID_STATE; // No loop task running
    }
    /* Cooperative stop — vTaskDelete here could kill the task while it
     * holds peers_lock, stranding the mutex. */
    _fpr_stop_task(&fpr_net.loop_task, &fpr_net.loop_stop_requested);
    return ESP_OK;
}

bool fpr_network_is_loop_task_running()
{
    return (fpr_net.loop_task != NULL);
}

// ========== ADVANCED SEND OPTIONS ==========
// literally the same thing as below
esp_err_t fpr_send_with_options(uint8_t *peer_address, void *data, int size, const fpr_send_options_t *options)
{
    ESP_RETURN_ON_FALSE(data != NULL && size > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid data or size");
    ESP_RETURN_ON_FALSE(options != NULL, ESP_ERR_INVALID_ARG, TAG, "Options cannot be NULL");
    
    // Check if network is paused
    if (fpr_net.paused) {
        ESP_LOGW(TAG, "Network is paused - send operation blocked");
        return ESP_ERR_INVALID_STATE;
    }
    
    const size_t PROTOCOL_SIZE = sizeof(((fpr_package_t *)0)->protocol);
    int data_remaining = size;
    bool single_packet = ((size_t)size <= PROTOCOL_SIZE);
    bool is_first_packet = true;
    uint8_t *data_ptr = (uint8_t *)data;
    esp_err_t last_result = ESP_OK;
    
    /* Sequence numbers are shared across senders — the app task, the
     * rx path's handshake replies, and reconnect tasks all bump this.
     * Take the (recursive) lock so cross-context sends can't mint
     * duplicates that a receiver's replay check would drop. */
    _peers_lock();
    uint32_t seq_num = ++fpr_net.tx_sequence_num;
    _peers_unlock();
    
    while (data_remaining > 0) {
        fpr_package_t package = {0};
        size_t chunk_size = ((size_t)data_remaining <= PROTOCOL_SIZE) ? (size_t)data_remaining : PROTOCOL_SIZE;
        bool is_last_packet = ((size_t)data_remaining <= PROTOCOL_SIZE);
        
        // Determine packet type
        if (single_packet) {
            package.package_type = FPR_PACKAGE_TYPE_SINGLE;
        } else if (is_first_packet) {
            package.package_type = FPR_PACKAGE_TYPE_START;
        } else if (is_last_packet) {
            package.package_type = FPR_PACKAGE_TYPE_END;
        } else {
            package.package_type = FPR_PACKAGE_TYPE_CONTINUED;
        }
        
        package.id = options->package_id;
        package.payload_size = (uint16_t)chunk_size;  // Track actual bytes in this packet
        package.sequence_num = seq_num;               // Sequence number for replay protection
        memcpy(&package.protocol, data_ptr, chunk_size);
        
        // Initialize routing fields
        memcpy(package.origin_mac, fpr_net.mac, 6);
        if (peer_address) {
            memcpy(package.dest_mac, peer_address, 6);
        } else {
            memset(package.dest_mac, 0xFF, 6);
        }
        package.hop_count = 0;
        package.max_hops = options->max_hops > 0 ? options->max_hops : FPR_DEFAULT_MAX_HOPS;
        package.version = FPR_NETWORK_VERSION;  // Set protocol version
        
        last_result = esp_now_send(peer_address, (const uint8_t *)&package, sizeof(package));
        if (last_result == ESP_OK) {
            fpr_net.stats.packets_sent++;
        } else {
            fpr_net.stats.send_failures++;
            // Log specific error for debugging
            if (last_result == ESP_ERR_ESPNOW_NO_MEM) {
                ESP_LOGW(TAG, "ESP-NOW buffer full (NO_MEM), try reducing send rate or increasing receive processing");
            }
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "esp_now_send failed: %s (0x%x)", esp_err_to_name(last_result), last_result);
            #endif
            return last_result; // Fail early on send error
        }

        data_ptr += chunk_size;
        data_remaining -= (int)chunk_size;
        is_first_packet = false;
        
        // Small delay between fragments to prevent overwhelming the receiver
        // Only needed for multi-packet transfers
        if (!single_packet && data_remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return last_result;
}

// current does not support bigger than default size. Would need to implement fragmentation later.
static esp_err_t fpr_network_send_helper(uint8_t *peer_address, void *data, int size, fpr_package_id_t package_id) 
{
    fpr_send_options_t options = {
        .package_id = package_id,
        .max_hops = FPR_DEFAULT_MAX_HOPS
    };
    return fpr_send_with_options(peer_address, data, size, &options);
}

esp_err_t fpr_network_send_to_peer(uint8_t *peer_address, void *data, int size, fpr_package_id_t package_id)
{
    ESP_RETURN_ON_FALSE(peer_address != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer address is NULL");

    // Validate peer exists and is connected (unless it's a control packet for handshake)
    if (package_id != FPR_PACKET_ID_CONTROL) {
        _peers_lock();
        FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_address);
        if (peer == NULL) {
            _peers_unlock();
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Attempting to send to unknown peer " MACSTR, MAC2STR(peer_address));
            #endif
            return ESP_ERR_NOT_FOUND;
        }
        if (!peer->is_connected) {
            _peers_unlock();
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Attempting to send to disconnected peer %s (" MACSTR ")", peer->name, MAC2STR(peer_address));
            #endif
            return ESP_ERR_INVALID_STATE;
        }
        _peers_unlock();
    }

    return fpr_network_send_helper(peer_address, data, size, package_id);
}

esp_err_t fpr_network_broadcast(void *data, int size, fpr_package_id_t package_id)
{
    uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    return fpr_network_send_helper(broadcast_mac, data, size, package_id);
}

// Create connection info with optional security keys
// used externally
fpr_connect_t make_fpr_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk) 
{
    fpr_connect_t info = {0}; // Zero-initialize
    _safe_string_copy(info.name, fpr_net.name, sizeof(info.name));
    memcpy(info.peer_info.peer_addr, fpr_net.mac, 6);
    fpr_set_peer_info(&info.peer_info); // Initialize peer_info properly
    info.visibility = fpr_net.access_state;
    
    // Include PWK if requested and available
    if (include_pwk && pwk) {
        memcpy(info.pwk, pwk, FPR_KEY_SIZE);
        info.has_pwk = true;
    } else {
        info.has_pwk = false;
    }
    
    // Include LWK if requested and available
    if (include_lwk && lwk) {
        memcpy(info.lwk, lwk, FPR_KEY_SIZE);
        info.has_lwk = true;
    } else {
        info.has_lwk = false;
    }
    
    return info;
}

static fpr_connect_t make_fpr_info() 
{
    // No keys by default
    return make_fpr_info_with_keys(false, false, NULL, NULL);
}

//extern 
esp_err_t fpr_network_send_device_info(uint8_t *peer_address)
{
    fpr_connect_t info = make_fpr_info();
    return fpr_network_send_to_peer(peer_address, (void *)&info, sizeof(info), FPR_PACKET_ID_CONTROL);
}
// extern 
esp_err_t fpr_network_broadcast_device_info()
{
    fpr_connect_t info = make_fpr_info();
    return fpr_network_broadcast((void *)&info, sizeof(info), FPR_PACKET_ID_CONTROL);
}

int fpr_network_get_peer_count(void)
{
    _peers_lock();
    int count = hashmap_size(&fpr_net.peers_map);
    _peers_unlock();
    return count;
}

void fpr_network_set_permission_state(fpr_visibility_t state)
{
    fpr_net.access_state = state;
}

fpr_visibility_t fpr_network_get_permission_state(void)
{
    return fpr_net.access_state;
}

/* Collect (and reference) every peer store under the lock so the user
 * callback can run unlocked — hashmap_foreach only tolerates removal
 * of the entry currently being visited, but a user callback may remove
 * any peer or clear the map outright. */
typedef struct {
    FPR_STORE_HASH_TYPE **stores;
    size_t cap;
    size_t n;
} _peer_snapshot_ctx_t;

static void _peer_snapshot_collect(void *key, void *value, void *user_data)
{
    (void)key;
    _peer_snapshot_ctx_t *ctx = (_peer_snapshot_ctx_t *)user_data;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    if (peer != NULL && ctx->n < ctx->cap) {
        peer->refcount++;
        ctx->stores[ctx->n++] = peer;
    }
}

size_t fpr_network_get_peers(fpr_data_receive_cb_t callback, void *user_data)
{
    if (callback == NULL) {
        return 0;
    }
    _peers_lock();
    size_t n = hashmap_size(&fpr_net.peers_map);
    if (n == 0) {
        _peers_unlock();
        return 0;
    }
    FPR_STORE_HASH_TYPE **stores =
        heap_caps_calloc(n, sizeof(*stores), MALLOC_CAP_DEFAULT);
    if (stores == NULL) {
        _peers_unlock();
        return 0;
    }
    _peer_snapshot_ctx_t ctx = { .stores = stores, .cap = n, .n = 0 };
    hashmap_foreach(&fpr_net.peers_map, _peer_snapshot_collect, &ctx);
    n = ctx.n;
    _peers_unlock();

    /* Unlocked callbacks: each store is refcounted, so the pointer stays
     * valid even if the callback removes the peer mid-iteration (the
     * store is just marked dead, freed at release). */
    for (size_t i = 0; i < n; i++) {
        callback(stores[i]->peer_info.peer_addr, stores[i], user_data);
        _peer_store_release(stores[i]);
    }
    heap_caps_free(stores);
    return n;
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

// ========== VERSION INFO API ==========

code_version_t fpr_get_protocol_version(void)
{
    return FPR_NETWORK_VERSION;
}

void fpr_get_protocol_version_string(char *buf, size_t buf_size)
{
    if (buf && buf_size > 0) {
        snprintf(buf, buf_size, "%" PRId32 ".%" PRId32 ".%" PRId32,
                 (uint32_t)CODE_VERSION_MAJOR(FPR_NETWORK_VERSION),
                 (uint32_t)CODE_VERSION_MINOR(FPR_NETWORK_VERSION),
                 (uint32_t)CODE_VERSION_PATCH(FPR_NETWORK_VERSION));
    }
}

// ========== NETWORK STATISTICS ==========

void fpr_get_network_stats(fpr_network_stats_t *stats)
{
    if (stats) {
        stats->packets_sent = fpr_net.stats.packets_sent;
        stats->packets_received = fpr_net.stats.packets_received;
        stats->packets_forwarded = fpr_net.stats.packets_forwarded;
        stats->packets_dropped = fpr_net.stats.packets_dropped;
        stats->send_failures = fpr_net.stats.send_failures;
        stats->replay_attacks_blocked = fpr_net.stats.replay_attacks_blocked;
        _peers_lock();
        stats->peer_count = hashmap_size(&fpr_net.peers_map);
        _peers_unlock();
    }
}

void fpr_reset_network_stats(void)
{
    memset(&fpr_net.stats, 0, sizeof(fpr_net.stats));
    ESP_LOGI(TAG, "Network statistics reset");
}

// ========== PEER DISCOVERY & INFO ==========

esp_err_t fpr_get_peer_info(uint8_t *peer_mac, fpr_peer_info_t *info)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && info != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    
    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }
    _copy_peer_to_info(peer, info);
    _peers_unlock();
    return ESP_OK;
}

typedef struct {
    fpr_peer_info_t *array;
    size_t max_peers;
    size_t count;
} peer_list_ctx_t;

static void _list_peers_callback(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    peer_list_ctx_t *ctx = (peer_list_ctx_t *)user_data;
    
    if (ctx->count < ctx->max_peers && peer) {
        _copy_peer_to_info(peer, &ctx->array[ctx->count]);
        ctx->count++;
    }
}

size_t fpr_list_all_peers(fpr_peer_info_t *peer_array, size_t max_peers)
{
    if (!peer_array || max_peers == 0) {
        return 0;
    }
    
    peer_list_ctx_t ctx = {
        .array = peer_array,
        .max_peers = max_peers,
        .count = 0
    };

    _peers_lock();
    hashmap_foreach(&fpr_net.peers_map, _list_peers_callback, &ctx);
    _peers_unlock();
    return ctx.count;
}

// ========== ROUTE TABLE MANAGEMENT ==========

typedef struct {
    uint32_t stale_timeout_ms;
    size_t removed_count;
} route_cleanup_ctx_t;

static void _cleanup_stale_routes_callback(void *key, void *value, void *user_data)
{
    uint8_t *mac = (uint8_t *)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    route_cleanup_ctx_t *ctx = (route_cleanup_ctx_t *)user_data;

    if (peer) {
        int64_t age = esp_timer_get_time() - peer->last_seen;
        uint64_t age_ms = (uint64_t)US_TO_MS(age);
        if (age_ms > ctx->stale_timeout_ms) {
            ESP_LOGI(TAG, "Removing stale route to " MACSTR " (age: %llu ms)",
                     MAC2STR(mac), (unsigned long long)age_ms);
            _remove_peer_internal(mac);
            ctx->removed_count++;
        }
    }
}

static void _cleanup_stale_route_entry(void *key, void *value, void *user_data)
{
    uint8_t *mac = (uint8_t *)key;
    fpr_route_entry_t *route = (fpr_route_entry_t *)value;
    route_cleanup_ctx_t *ctx = (route_cleanup_ctx_t *)user_data;

    if (route) {
        int64_t age = esp_timer_get_time() - route->last_seen;
        uint64_t age_ms = (uint64_t)US_TO_MS(age);
        if (age_ms > ctx->stale_timeout_ms) {
            ESP_LOGI(TAG, "Removing stale extender route to " MACSTR " (age: %llu ms)",
                     MAC2STR(mac), (unsigned long long)age_ms);
            hashmap_remove(&fpr_net.routes_map, mac);
            heap_caps_free(route);
            ctx->removed_count++;
        }
    }
}

size_t fpr_cleanup_stale_routes(uint32_t timeout_ms)
{
    route_cleanup_ctx_t ctx = {
        .stale_timeout_ms = timeout_ms,
        .removed_count = 0
    };

    _peers_lock();
    hashmap_foreach(&fpr_net.peers_map, _cleanup_stale_routes_callback, &ctx);
    hashmap_foreach(&fpr_net.routes_map, _cleanup_stale_route_entry, &ctx);
    _peers_unlock();

    if (ctx.removed_count > 0) {
        ESP_LOGI(TAG, "Cleaned up %zu stale routes", ctx.removed_count);
    }
    
    return ctx.removed_count;
}

void fpr_print_route_table(void)
{
    _peers_lock();
    size_t peer_count = hashmap_size(&fpr_net.peers_map);
    ESP_LOGI(TAG, "========== ROUTE TABLE (%zu peers) ==========", peer_count);

    if (peer_count == 0) {
        _peers_unlock();
        ESP_LOGI(TAG, "  (empty)");
        return;
    }
    _peers_unlock();

    fpr_peer_info_t *peers = heap_caps_malloc(peer_count * sizeof(fpr_peer_info_t), MALLOC_CAP_DEFAULT);
    if (!peers) {
        ESP_LOGE(TAG, "Failed to allocate memory for route table");
        return;
    }

    size_t actual = fpr_list_all_peers(peers, peer_count);
    for (size_t i = 0; i < actual; i++) {
        const char *state_str = "UNKNOWN";
        switch (peers[i].state) {
            case FPR_PEER_STATE_DISCOVERED: state_str = "discovered"; break;
            case FPR_PEER_STATE_PENDING: state_str = "PENDING"; break;
            case FPR_PEER_STATE_CONNECTED: state_str = "CONNECTED"; break;
            case FPR_PEER_STATE_REJECTED: state_str = "rejected"; break;
            case FPR_PEER_STATE_BLOCKED: state_str = "BLOCKED"; break;
        }
        
        ESP_LOGI(TAG, "  %s (" MACSTR ") | Hops: %d | RSSI: %d dBm | Age: %llu ms | Pkts: %lu | %s",
                 peers[i].name,
                 MAC2STR(peers[i].mac),
                 peers[i].hop_count,
                 peers[i].rssi,
                 peers[i].last_seen_ms,
                 peers[i].packets_received,
                 state_str);
    }
    
    heap_caps_free(peers);
    ESP_LOGI(TAG, "============================================");
}

// ========== CONNECTION CONTROL API IMPLEMENTATION ==========

bool fpr_network_get_data_from_peer(uint8_t *peer_mac, void *data, int data_size, TickType_t timeout)
{
    if (peer_mac == NULL) {
        return false;
    }
    /* Acquire a store reference instead of holding peers_lock across the
     * (possibly blocking) receive — holding it would starve the RX path
     * that fills the queue. The reference keeps the store and its queue
     * alive even if the peer is removed concurrently. */
    FPR_STORE_HASH_TYPE *peer = _peer_store_acquire(peer_mac);
    bool result = false;
    if (peer && data && data_size > 0) {
        const size_t CHUNK_CAP = sizeof(((fpr_package_t *)0)->protocol);
        fpr_package_t pkg;
        size_t offset = 0;
        bool expecting_more = false;
        /* Bound each queue wait: a peer removed while we block marks the
         * store dead, and after deinit nothing produces — without the
         * dead check a portMAX_DELAY reader would hang forever holding
         * the store. */
        TickType_t waited = 0;

        while (1) {
            if (peer->dead) {
                goto out;
            }
            TickType_t slice = pdMS_TO_TICKS(25);
            if (timeout != portMAX_DELAY && timeout < slice) {
                slice = timeout;
            }
            if (xQueueReceive(peer->response_queue, &pkg, slice) != pdPASS) {
                if (timeout != portMAX_DELAY) {
                    waited += slice;
                    if (waited >= timeout) {
                        goto out;
                    }
                }
                continue;
            }
            // Use payload_size if set, otherwise fall back to CHUNK_CAP for backwards compatibility
            size_t actual_payload = (pkg.payload_size > 0 && pkg.payload_size <= CHUNK_CAP) 
                                    ? pkg.payload_size : CHUNK_CAP;
            
            // Calculate how much to copy - don't exceed remaining buffer space
            size_t remaining_space = (size_t)data_size - offset;
            size_t copy_size = (remaining_space < actual_payload) ? remaining_space : actual_payload;
    
            switch (pkg.package_type) {
                case FPR_PACKAGE_TYPE_SINGLE:
                    // Single packet - copy the actual payload size
                    copy_size = ((size_t)data_size < actual_payload) ? (size_t)data_size : actual_payload;
                    memcpy(data, &pkg.protocol, copy_size);
                    // Decrement queued packet count (complete packet consumed)
                    _peer_dec_queued(peer);
                    result = true;
                    goto out;  // Success - got complete single packet

                case FPR_PACKAGE_TYPE_START:
                    // Begin a multi-packet transfer - reset offset
                    offset = 0;
                    expecting_more = true;
                    remaining_space = (size_t)data_size;
                    copy_size = (remaining_space < actual_payload) ? remaining_space : actual_payload;
                    memcpy((uint8_t*)data + offset, &pkg.protocol, copy_size);
                    offset += copy_size;
                    break;
    
                case FPR_PACKAGE_TYPE_CONTINUED:
                    if (!expecting_more) {
                        continue; // out of order, skip
                    }
                    memcpy((uint8_t*)data + offset, &pkg.protocol, copy_size);
                    offset += copy_size;
                    break;
    
                case FPR_PACKAGE_TYPE_END:
                    if (!expecting_more) {
                        // Orphaned END was counted as a complete packet when
                        // queued; dropping it here must decrement too or
                        // queued_packets desyncs from the real queue.
                        _peer_dec_queued(peer);
                        continue; // out of order, skip
                    }
                    memcpy((uint8_t*)data + offset, &pkg.protocol, copy_size);
                    offset += copy_size;
                    // Decrement queued packet count (complete multi-packet consumed)
                    _peer_dec_queued(peer);
                    // Success - got complete multi-packet transfer
                    result = true;
                    goto out;

                default:
                    // Unknown type, ignore
                    continue;
            }

            // If we've already filled the buffer, stop early
            if (offset >= (size_t)data_size) {
                result = true;
                goto out;
            }
        }
    }
out:
    _peer_store_release(peer);
    return result;
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
        result = xTaskCreatePinnedToCore(_fpr_client_reconnect_task, "FPR_Client_Reconnect", FPR_TASK_STACK_SIZE, NULL, FPR_TASK_PRIORITY, &fpr_net.reconnect_task, FPR_RECONNECT_TASK_CORE_PIN_VALUE);
    }
    else if (fpr_net.current_mode == FPR_MODE_HOST) {
        // Override protocol handler to keep connection logic active
        result = xTaskCreatePinnedToCore(_fpr_host_reconnect_task, "FPR_Host_Reconnect", FPR_TASK_STACK_SIZE, NULL, FPR_TASK_PRIORITY, &fpr_net.reconnect_task, FPR_RECONNECT_TASK_CORE_PIN_VALUE);
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
    _fpr_stop_task(&fpr_net.reconnect_task, &fpr_net.reconnect_stop_requested);

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

esp_err_t fpr_network_set_peer_queue_mode(uint8_t *peer_mac, fpr_queue_mode_t mode)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    if (peer) {
        peer->queue_mode = mode;
        _peers_unlock();
        ESP_LOGI(TAG, "Queue mode for peer " MACSTR " set to %s",
                 MAC2STR(peer_mac), mode == FPR_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
        return ESP_OK;
    }
    _peers_unlock();
    return ESP_ERR_NOT_FOUND;
}

uint32_t fpr_network_get_peer_queued_packets(uint8_t *peer_mac)
{
    if (peer_mac == NULL) {
        return 0;
    }

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    uint32_t count = peer ? peer->queued_packets : 0;
    _peers_unlock();
    return count;
}

// ========== PEER MANAGEMENT ENHANCEMENTS ==========

typedef struct {
    const char *name;
    uint8_t *mac_out;
    bool found;
} peer_name_search_ctx_t;

static void _find_peer_by_name_callback(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    peer_name_search_ctx_t *ctx = (peer_name_search_ctx_t *)user_data;
    
    if (peer && !ctx->found) {
        if (strncmp(peer->name, ctx->name, PEER_NAME_MAX_LENGTH) == 0) {
            memcpy(ctx->mac_out, peer->peer_info.peer_addr, MAC_ADDRESS_LENGTH);
            ctx->found = true;
        }
    }
}

esp_err_t fpr_get_peer_by_name(const char *peer_name, uint8_t *mac_out)
{
    ESP_RETURN_ON_FALSE(peer_name != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer name is NULL");
    ESP_RETURN_ON_FALSE(mac_out != NULL, ESP_ERR_INVALID_ARG, TAG, "MAC output buffer is NULL");
    
    peer_name_search_ctx_t ctx = {
        .name = peer_name,
        .mac_out = mac_out,
        .found = false
    };

    _peers_lock();
    hashmap_foreach(&fpr_net.peers_map, _find_peer_by_name_callback, &ctx);
    _peers_unlock();

    if (!ctx.found) {
        return ESP_ERR_NOT_FOUND;
    }
    
    return ESP_OK;
}

static void _clear_all_peers_callback(void *key, void *value, void *user_data)
{
    (void)user_data;
    uint8_t *mac = (uint8_t *)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    
    if (peer) {
        // Remove from ESP-NOW
        esp_now_del_peer(mac);
        _peer_store_free_or_defer(peer);
    }
}

esp_err_t fpr_clear_all_peers(void)
{
    _peers_lock();
    size_t peer_count = hashmap_size(&fpr_net.peers_map);

    if (peer_count == 0) {
        _peers_unlock();
        ESP_LOGI(TAG, "No peers to clear");
        return ESP_OK;
    }

    // Clean up each peer
    hashmap_foreach(&fpr_net.peers_map, _clear_all_peers_callback, NULL);

    // Clear the hashmap
    hashmap_clear(&fpr_net.peers_map);

    // Extender routes are useless without the peers they describe
    hashmap_foreach(&fpr_net.routes_map, _free_route_entry, NULL);
    hashmap_clear(&fpr_net.routes_map);
    _peers_unlock();

    ESP_LOGI(TAG, "Cleared %zu peers", peer_count);
    return ESP_OK;
}

bool fpr_is_peer_reachable(uint8_t *peer_mac, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, false, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGW(TAG, "Peer not found in peer map");
        return false;
    }

    // Check if peer was recently seen (within timeout)
    int64_t now_us = esp_timer_get_time();
    int64_t age_us = now_us - peer->last_seen;
    uint64_t age_ms = (uint64_t)US_TO_MS(age_us);
    int64_t initial_last_seen = peer->last_seen;
    _peers_unlock();

    if (age_ms <= timeout_ms) {
        return true;
    }

    // Send a ping (device info) and check response
    esp_err_t err = fpr_network_send_device_info(peer_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send ping to peer: %s", esp_err_to_name(err));
        return false;
    }

    // Wait for response by checking if last_seen timestamp updates.
    // Re-lookup each iteration: holding the store (or the lock) across
    // vTaskDelay would both UAF on concurrent removal and starve the RX
    // path that updates last_seen — the wait could never succeed.
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        _peers_lock();
        peer = _get_peer_from_map(peer_mac);
        bool responded = (peer && peer->last_seen > initial_last_seen);
        _peers_unlock();
        if (responded) {
            return true;  // Peer responded
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // Small delay between checks
    }

    return false;  // Timeout - peer did not respond
}