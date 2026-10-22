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
#include "fpr/fpr_transport.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_check.h"

// Protocol configuration
#define FPR_HASHMAP_INITIAL_SIZE 32
/* Drain window in fpr_deinit for ESP-NOW RX callbacks that
 * were already dispatched when the recv cb was unregistered — they
 * run on the wifi task, which outranks every deinit caller, so this
 * only has to cover a few scheduling opportunities. */
#define FPR_DEINIT_SETTLE_MS 50

static const char *TAG = "fpr";

static void _rx_dispatch(void *sink, const fpr_rx_frame_t *frame);
static void _tx_done_sink(void *sink, const uint8_t *mac, bool ok);

/* Cooperative-stop grace for the loop/reconnect tasks: they poll their
 * stop flag once per configured poll interval (power-mode stretched),
 * so the window must cover one worst-case iteration plus lock hold. */
static inline TickType_t _task_stop_grace(fpr_node_t *net)
{
    uint32_t interval =
        _power_interval(net, FPR_HOST_SCAN_POLL_INTERVAL_MS);
    /* Saturate: the power-mode multiplier can clamp the interval to
     * UINT32_MAX, and 2*interval+250 must not wrap into a hair-trigger
     * vTaskDelete fallback. */
    uint32_t grace_ms = interval > (UINT32_MAX - 250) / 2
                            ? UINT32_MAX : 2 * interval + 250;
    return pdMS_TO_TICKS(grace_ms);
}

/* Request a cooperative stop and join on the handle clearing its own
 * slot. vTaskDelete remains the fallback for a wedged task — but only
 * after a full grace window, because a task killed while holding
 * peers_lock strands the recursive mutex for every later caller. */
static void _fpr_task_stop(fpr_node_t *net, TaskHandle_t *handle, volatile bool *stop_flag)
{
    if (handle == NULL || *handle == NULL) {
        return;
    }
    TaskHandle_t target = *handle;
    *stop_flag = true;
    TickType_t deadline = xTaskGetTickCount() + _task_stop_grace(net);
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
static void _task_exit(fpr_node_t *net, TaskHandle_t *slot)
{
    _peers_lock(net);
    if (*slot == xTaskGetCurrentTaskHandle()) {
        *slot = NULL;
    }
    _peers_unlock(net);
    vTaskDelete(NULL);
}

// Helper: Add broadcast peer to the transport's peer table
// this needs to be called whenever we change mode (host/client/extender)
static esp_err_t _broadcast_peer_add(fpr_node_t *net, const char *mode_name)
{
    static const uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    const fpr_transport_t *t = _transport(net);
    if (t == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    t->del_peer(t->ctx, broadcast_mac);
    esp_err_t err = t->add_peer(t->ctx, broadcast_mac, 0, false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Broadcast peer added for %s", mode_name);
    }
    return err;
}

/* Initialize caller-provided node storage — shared by fpr_create()
 * (heap node) and by fixtures with static storage. Callbacks are not
 * set here: create() copies cfg->callbacks, bare init leaves them
 * zero (fpr_set_callbacks installs them later). */
esp_err_t fpr_init_at(fpr_node_t *net, const char *name, const fpr_init_config_t *config)
{
    ESP_RETURN_ON_FALSE(net != NULL, ESP_ERR_INVALID_ARG, TAG, "Node is NULL");
    ESP_RETURN_ON_FALSE(name != NULL, ESP_ERR_INVALID_ARG, TAG, "Name is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    ESP_RETURN_ON_FALSE(strlen(name) < sizeof(net->name), ESP_ERR_INVALID_ARG, TAG, "Name too long");
    ESP_RETURN_ON_FALSE(net->state == FPR_STATE_UNINITIALIZED, ESP_ERR_INVALID_STATE, TAG, "Network already initialized");

    net->transport = config->transport ? config->transport
                                          : fpr_transport_espnow();
    const fpr_transport_t *t = net->transport;
    ESP_RETURN_ON_FALSE(t->mtu > sizeof(fpr_package_t), ESP_ERR_INVALID_SIZE,
                        TAG, "Transport MTU %u too small for fpr_package_t (%u)",
                        (unsigned)t->mtu, (unsigned)sizeof(fpr_package_t));
    ESP_RETURN_ON_ERROR(t->get_mac(t->ctx, net->mac), TAG, "Failed to read MAC address");
    
    strncpy(net->name, name, sizeof(net->name) - 1);
    net->name[sizeof(net->name) - 1] = '\0';
    // Store channel and power mode config
    net->channel = config->channel;
    net->power_mode = config->power_mode;
    net->default_queue_mode = config->queue_mode;

    net->access_state = FPR_VISIBILITY_PUBLIC;
    
    // Initialize default host config
    net->host_config.max_peers = 32;  // 32 by default
    net->host_config.connection_mode = FPR_CONNECTION_AUTO;
    net->host_config.request_cb = NULL;
    
    // Initialize default client config
    net->client_config.connection_mode = FPR_CONNECTION_AUTO;
    net->client_config.discovery_cb = NULL;

    // Initialize security
    net->host_pwk_valid = false;
    
    // Initialize sequence number counter
    net->tx_sequence_num = 0;

    /* peers_lock is process-lifetime: created lazily here and never
     * deleted. A callback or reader may legitimately reach for it after
     * deinit (radio callbacks already dispatched, a blocked
     * get_data_from_peer waking late) — deleting the semaphore under a
     * waiter is UB, and the guarded objects are left in hashmap's
     * safe-empty state anyway. */
    if (net->peers_lock == NULL) {
        net->peers_lock = xSemaphoreCreateRecursiveMutex();
        if (net->peers_lock == NULL) {
            ESP_LOGE(TAG, "Failed to allocate peers lock");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!hashmap_init(&net->peers_map, FPR_HASHMAP_INITIAL_SIZE, mac_hash, mac_equals)) {
        ESP_LOGE(TAG, "Failed to allocate peers map");
        return ESP_ERR_NO_MEM;
    }
    if (!hashmap_init(&net->routes_map, FPR_HASHMAP_INITIAL_SIZE, mac_hash, mac_equals)) {
        ESP_LOGE(TAG, "Failed to allocate routes map");
        hashmap_free(&net->peers_map);
        return ESP_ERR_NO_MEM;
    }
    /* Bring the radio up with the dispatch sinks installed. RX frames
     * arriving before set_mode installs a handler are dropped, matching
     * the old "no recv cb registered until start" behaviour. */
    esp_err_t radio_err = t->open(t->ctx, _rx_dispatch, _tx_done_sink, net);
    if (radio_err != ESP_OK) {
        /* The maps are already allocated — free them or every init retry
         * on a degraded radio leaks the bucket arrays. */
        hashmap_free(&net->routes_map);
        hashmap_free(&net->peers_map);
        ESP_LOGE(TAG, "Failed to open transport: %s", esp_err_to_name(radio_err));
        return radio_err;
    }

    /* Channel select and the broadcast peer need a live radio: the
     * transport contract is that every vtable op fails while closed,
     * so both run only after open() succeeds. The broadcast peer is
     * also re-added by fpr_set_mode for client/host/extender — the
     * init-time add covers modes that never take a branch there
     * (BROADCAST/DEFAULT). */
    if (config->channel >= 1 && config->channel <= 14) {
        esp_err_t err = t->set_channel(t->ctx, config->channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi channel %d: %s", config->channel, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi channel set to %d", config->channel);
        }
    }
    if (_broadcast_peer_add(net, "default") != ESP_OK) {
        ESP_LOGW(TAG, "Broadcast peer add failed at init");
    }

    net->state = FPR_STATE_INITIALIZED;
    net->paused = false;
    
    ESP_LOGI(TAG, "FPR Network initialized: %s (" MACSTR ") ch=%d pwr=%s queue=%s", 
             net->name, MAC2STR(net->mac),
             net->channel ? net->channel : 0,
             net->power_mode == FPR_POWER_LOW ? "LOW" : "NORMAL",
             net->default_queue_mode == FPR_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
    return ESP_OK;
}

/* Deinit peer sweep: free each store (or mark dead for a live reader
 * to release). Runs after transport close() — the peer table went down
 * with the radio, so there is nothing to del_peer against. Only used
 * by fpr_deinit — kept file-local. */
static void _peer_cleanup_cb(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    if (peer) {
        _peer_store_free_or_defer(peer);
    }
}

static void _reset_all_peers(fpr_node_t *net)
{
    hashmap_foreach(&net->peers_map, _peer_cleanup_cb, net);
}

esp_err_t fpr_deinit(fpr_node_t *net)
{
    /* Quiesce gate FIRST — peer adds and extender route learning reject
     * immediately; a callback still in flight sees UNINITIALIZED when it
     * re-checks state under the lock, before any teardown below runs. */
    _peers_lock(net);
    net->state = FPR_STATE_UNINITIALIZED;
    _peers_unlock(net);

    /* Ask the internal tasks to exit rather than vTaskDelete them: a
     * task killed while holding peers_lock strands the recursive mutex
     * for every later caller (FreeRTOS never releases a dead task's
     * mutexes). The helper joins on the task clearing its own handle
     * slot inside a poll-interval grace window. */
    net->loop_armed = false;
    net->reconnect_armed = false;
    _fpr_task_stop(net, &net->reconnect_task, &net->reconnect_stop_requested);
    _fpr_task_stop(net, &net->loop_task, &net->loop_stop_requested);

    /* close() unregisters the recv sink before tearing the radio down:
     * nothing new is dispatched on the wifi task after it returns, so
     * only callbacks already in flight remain below. A deinit before
     * any successful init has no transport yet — nothing to close. */
    esp_err_t radio_result = net->transport
        ? net->transport->close(net->transport->ctx) : ESP_OK;

    /* Drain the callbacks dispatched before the transport closed: they
     * finish on the still-valid lock + maps (writes gated by the state
     * check; reads still see valid entries). The wifi task outranks
     * every deinit caller, so a modest delay guarantees it got
     * scheduled. */
    vTaskDelay(pdMS_TO_TICKS(FPR_DEINIT_SETTLE_MS));

    /* Now free under the lock. A pathological straggler that still
     * wakes late re-locks a LIVE mutex and reads maps that
     * hashmap_free leaves in a safe empty state — every map op is a
     * no-op returning NULL on NULL buckets — and each deinit pass
     * restores exactly that state for the next cycle. */
    _peers_lock(net);
    _reset_all_peers(net);
    hashmap_free(&net->peers_map);
    hashmap_foreach(&net->routes_map, _route_free_cb, NULL);
    hashmap_free(&net->routes_map);
    _peers_unlock(net);

    /* peers_lock survives deinit: a straggler callback or a blocked
     * get_data_from_peer reader may still reach for it after the memset
     * — deleting the semaphore under a waiter would be use-after-free.
     * For heap nodes (owns_storage) fpr_destroy frees it once the
     * caller guarantees callbacks have drained; caller-provided
     * (static) nodes keep the lock alive — the storage owner decides. */
    SemaphoreHandle_t kept_lock = net->peers_lock;
    bool owned = net->owns_storage;
    memset(net, 0, sizeof(*net));
    net->state = FPR_STATE_UNINITIALIZED;
    net->peers_lock = kept_lock;
    net->owns_storage = owned;

    return radio_result;
}

/* Allocate + init a node in one call — the public entry point. */
esp_err_t fpr_create(const fpr_node_config_t *cfg, fpr_node_t **out)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && out != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    fpr_node_t *net = heap_caps_calloc(1, sizeof(*net), MALLOC_CAP_8BIT);
    if (net == NULL) {
        return ESP_ERR_NO_MEM;
    }
    net->owns_storage = true;
    net->callbacks = cfg->callbacks;
    esp_err_t err = fpr_init_at(net, cfg->name, &cfg->init);
    if (err != ESP_OK) {
        if (net->peers_lock != NULL) {
            vSemaphoreDelete(net->peers_lock);
        }
        heap_caps_free(net);
        return err;
    }
    *out = net;
    return ESP_OK;
}

void fpr_destroy(fpr_node_t *net)
{
    if (net == NULL) {
        return;
    }
    /* Legal only after fpr_deinit(): the transport close there has
     * drained every callback, so freeing the lock is now safe for an
     * owned node. */
    if (net->state != FPR_STATE_UNINITIALIZED) {
        fpr_deinit(net);
    }
    if (!net->owns_storage) {
        /* Caller-provided storage (fpr_init_at): the node is left
         * deinitialized and peers_lock stays alive — freeing either is
         * the storage owner's call. */
        return;
    }
    if (net->peers_lock != NULL) {
        vSemaphoreDelete(net->peers_lock);
    }
    heap_caps_free(net);
}

/* Runtime callback table swap — the registered pointers may fire from
 * the transport RX context, so callers must keep them valid until
 * fpr_stop()/fpr_deinit() drains it. */
esp_err_t fpr_set_callbacks(fpr_node_t *net, const fpr_node_callbacks_t *callbacks)
{
    ESP_RETURN_ON_FALSE(net != NULL, ESP_ERR_INVALID_ARG, TAG, "Node is NULL");
    _peers_lock(net);
    if (callbacks != NULL) {
        net->callbacks = *callbacks;
    } else {
        net->callbacks = (fpr_node_callbacks_t){0};
    }
    _peers_unlock(net);
    return ESP_OK;
}

/* Transport TX-completion sink — debug logging only, same as the old
 * esp_now send-cb. */
static void _tx_done_sink(void *sink, const uint8_t *mac, bool ok)
{
    (void)sink;
    (void)mac;
    #if (FPR_DEBUG == 1)
    if (ok) {
        ESP_LOGI(TAG, "Data sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send data");
    }
    #endif
}

/* Transport RX sink: every received frame lands here and is handed to
 * the mode protocol handler installed by fpr_set_mode. Runs on
 * the transport's RX context (the wifi task under ESP-NOW). */
static void _rx_dispatch(void *sink, const fpr_rx_frame_t *frame)
{
    fpr_node_t *net = (fpr_node_t *)sink;
    if (net && frame && frame->data && net->rx_handler) {
        net->rx_handler(net, frame, frame->data, (int)frame->len);
    }
}

esp_err_t fpr_start(fpr_node_t *net)
{
    ESP_RETURN_ON_FALSE(net->state == FPR_STATE_INITIALIZED || net->state == FPR_STATE_STOPPED,
                        ESP_ERR_INVALID_STATE, TAG, "Network not initialized (state %d)", net->state);
    ESP_RETURN_ON_FALSE(net->transport->ready(net->transport->ctx),
                        ESP_ERR_INVALID_STATE, TAG, "Radio is not initialized or in a usable mode");

    /* A node that never picked a mode comes up as a CLIENT — modeless
     * started nodes have no RX handler and would silently drop frames.
     * An explicit fpr_set_mode() before start is honored. */
    if (net->current_mode == FPR_MODE_DEFAULT) {
        fpr_set_mode(net, FPR_MODE_CLIENT);
    }

    _peers_lock(net);
    net->state = FPR_STATE_STARTED;
    net->paused = false;
    _peers_unlock(net);

    ESP_LOGI(TAG, "FPR Network started with MAC: " MACSTR, MAC2STR(net->mac));
    return ESP_OK;
}

esp_err_t fpr_stop(fpr_node_t *net)
{
    if (net->state == FPR_STATE_STOPPED || net->state == FPR_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Network already stopped or not initialized");
        return ESP_OK;
    }
    
    _peers_lock(net);
    net->state = FPR_STATE_STOPPED;
    net->paused = false;
    /* Stop means no periodic work: a stopped node must not keep
     * broadcasting or reconnecting via fpr_poll / the node tasks. */
    net->loop_armed = false;
    net->reconnect_armed = false;
    _peers_unlock(net);
    ESP_LOGI(TAG, "Network stopped");
    return ESP_OK;
}

/* Install the mode's protocol handler for _rx_dispatch. An assignment
 * now — the transport sink is fixed — kept as a function so set_mode
 * stays readable. */
static esp_err_t _set_rx_handler(fpr_node_t *net,
                                 void (*handler)(fpr_node_t *, const fpr_rx_frame_t *,
                                                 const uint8_t *, int))
{
    net->rx_handler = handler;
    return ESP_OK;
}

void _client_loop_step(fpr_node_t *net)
{
    /* The RX handler does all the work; the loop interval only paces it. */
    (void)net;
}

void _host_loop_step(fpr_node_t *net)
{
    /* Announce presence — the RX handler does the connection work. */
    fpr_broadcast_device_info(net);
}

static void _loop_step(fpr_node_t *net)
{
    if (net->current_mode == FPR_MODE_HOST) {
        _host_loop_step(net);
    } else {
        _client_loop_step(net);
    }
}

/* Shared pacing for both periodic halves — power-mode stretched. */
static uint32_t _step_interval_ms(fpr_node_t *net)
{
    return _power_interval(net, net->current_mode == FPR_MODE_HOST
                                  ? FPR_HOST_SCAN_POLL_INTERVAL_MS
                                  : FPR_CLIENT_WAIT_CHECK_INTERVAL_MS);
}

static void _reconnect_step(fpr_node_t *net, uint32_t now_ms)
{
    if (net->current_mode == FPR_MODE_HOST) {
        _host_reconnect_step(net, now_ms);
    } else {
        _client_reconnect_step(net, now_ms);
    }
}

static uint32_t _poll_loop(fpr_node_t *net, uint32_t now_ms);
static uint32_t _poll_reconnect(fpr_node_t *net, uint32_t now_ms);

/* Loop task body: arg is the node. Exit when disarmed (duration bound
 * reached) or stop-requested — poll() disarms on the deadline. */
static void _fpr_loop_task_entry(void *arg)
{
    fpr_node_t *net = (fpr_node_t *)arg;
    ESP_LOGI(TAG, "%s loop task started",
             net->current_mode == FPR_MODE_HOST ? "Host" : "Client");

    while (!net->loop_stop_requested && net->loop_armed) {
        uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t wait_ms = _poll_loop(net, now_ms);
        if (wait_ms == 0 || wait_ms == UINT32_MAX) {
            wait_ms = _step_interval_ms(net);
        }
        /* wait_ms is relative to now, not last_wake — vTaskDelayUntil
         * would wake early by (now - last_wake) and spin re-polls. */
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    _task_exit(net, &net->loop_task);
}

/* Same pattern for the reconnect/keepalive task — runs only the
 * reconnect half of the poll so the two tasks never double-step. */
static void _fpr_reconnect_task_entry(void *arg)
{
    fpr_node_t *net = (fpr_node_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (!net->reconnect_stop_requested && net->reconnect_armed) {
        uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        _poll_reconnect(net, now_ms);
        vTaskDelayUntil(&last_wake,
                        pdMS_TO_TICKS(_step_interval_ms(net)));
    }

    _task_exit(net, &net->reconnect_task);
}

void fpr_set_mode(fpr_node_t *net, fpr_mode_type_t mode)
{
    net->current_mode = mode;
    /* Mesh forwarding only runs in extender mode. */
    net->routing_enabled = (mode == FPR_MODE_EXTENDER);

    /* Client/host/extender each need the broadcast peer registered and
     * the mode's RX handler installed. Failures only get logged: the
     * signature is void, and a missing broadcast peer or a stale RX
     * handler is diagnosable rather than silently misdispatching. */
    if (mode == FPR_MODE_CLIENT) {
        if (_broadcast_peer_add(net, "client") != ESP_OK) {
            ESP_LOGW(TAG, "Broadcast peer add failed for client mode");
        }
        if (_set_rx_handler(net, _rx_client) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install client RX handler");
        }
    }
    else if (mode == FPR_MODE_HOST) {
        if (_broadcast_peer_add(net, "host") != ESP_OK) {
            ESP_LOGW(TAG, "Broadcast peer add failed for host mode");
        }
        // Generate host PWK for secure connections — under the lock so
        // rx callbacks never observe a valid flag ahead of the key bytes
        _peers_lock(net);
        if (fpr_security_generate_pwk(net->host_pwk) == ESP_OK) {
            net->host_pwk_valid = true;
            ESP_LOGI(TAG, "Host mode set with generated PWK");
        } else {
            ESP_LOGE(TAG, "Failed to generate PWK for host mode");
        }
        _peers_unlock(net);
        if (_set_rx_handler(net, _rx_host) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install host RX handler");
        }
    }
    else if (mode == FPR_MODE_EXTENDER) {
        if (_broadcast_peer_add(net, "extender") != ESP_OK) {
            ESP_LOGW(TAG, "Broadcast peer add failed for extender mode");
        }
        if (_set_rx_handler(net, _rx_extender) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install extender RX handler");
        }
    }
    else {
        /* FPR_MODE_DEFAULT/FPR_MODE_BROADCAST: accepted but have no
         * handler of their own — the previous mode's RX stays armed. */
        ESP_LOGW(TAG, "Mode %d has no handler — RX callback unchanged", (int)mode);
    }
}

fpr_mode_type_t fpr_get_mode(fpr_node_t *net)
{
    return net->current_mode;
}

// ========== PERIODIC WORK: poll + task control ==========

esp_err_t fpr_start_loop(fpr_node_t *net, uint32_t duration_ms)
{
    if (net->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (net->current_mode == FPR_MODE_EXTENDER) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (net->current_mode != FPR_MODE_CLIENT && net->current_mode != FPR_MODE_HOST) {
        return ESP_ERR_INVALID_STATE;
    }
    net->loop_armed = true;
    /* Due at the next poll call; the deadline is anchored against the
     * poll clock on the first run (duration_ms == 0 means run forever). */
    net->loop_next_due_ms = 0;
    net->loop_duration_ms = duration_ms;
    net->loop_deadline_ms = 0;
    return ESP_OK;
}

esp_err_t fpr_stop_loop(fpr_node_t *net)
{
    net->loop_armed = false;
    return ESP_OK;
}

esp_err_t fpr_start_reconnect(fpr_node_t *net)
{
    if (net->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (net->current_mode != FPR_MODE_CLIENT && net->current_mode != FPR_MODE_HOST) {
        return ESP_ERR_INVALID_STATE;
    }
    net->reconnect_armed = true;
    net->reconnect_next_due_ms = 0;
    return ESP_OK;
}

esp_err_t fpr_stop_reconnect(fpr_node_t *net)
{
    net->reconnect_armed = false;
    return ESP_OK;
}

static uint32_t _poll_loop(fpr_node_t *net, uint32_t now_ms)
{
    uint32_t next = UINT32_MAX;

    if (net->loop_armed) {
        if (net->loop_deadline_ms == 0 && net->loop_duration_ms) {
            net->loop_deadline_ms = now_ms + net->loop_duration_ms;
        }
        if (net->loop_deadline_ms && (int32_t)(now_ms - net->loop_deadline_ms) >= 0) {
            net->loop_armed = false;
        /* next_due == 0 is the "armed, due at next poll" sentinel from
         * fpr_start_loop — comparing it as a timestamp would read as
         * far-future once now_ms passes 2^31. A computed due can only
         * hit 0 once per epoch wrap, costing at most one early step. */
        } else if (net->loop_next_due_ms == 0 ||
                   (int32_t)(now_ms - net->loop_next_due_ms) >= 0) {
            _loop_step(net);
            net->loop_next_due_ms = now_ms + _step_interval_ms(net);
        }
        if (net->loop_armed) {
            uint32_t due = net->loop_next_due_ms - now_ms;
            if (net->loop_deadline_ms) {
                uint32_t dl = net->loop_deadline_ms - now_ms;
                if (dl < due) {
                    due = dl;
                }
            }
            if (due < next) {
                next = due;
            }
        }
    }

    return next;
}

static uint32_t _poll_reconnect(fpr_node_t *net, uint32_t now_ms)
{
    uint32_t next = UINT32_MAX;

    if (net->reconnect_armed) {
        /* Same 0 sentinel as the loop half (see _poll_loop). */
        if (net->reconnect_next_due_ms == 0 ||
            (int32_t)(now_ms - net->reconnect_next_due_ms) >= 0) {
            _reconnect_step(net, now_ms);
            net->reconnect_next_due_ms = now_ms + _step_interval_ms(net);
        }
        uint32_t due = net->reconnect_next_due_ms - now_ms;
        if (due < next) {
            next = due;
        }
    }

    return next;
}

uint32_t fpr_poll(fpr_node_t *net, uint32_t now_ms)
{
    uint32_t l = _poll_loop(net, now_ms);
    uint32_t r = _poll_reconnect(net, now_ms);
    return l < r ? l : r;
}

/* Spawn one periodic task on caller-owned static storage. The handle
 * lands in net->loop_task / net->reconnect_task so the cooperative
 * stop path is shared with fpr_stop(). */
static esp_err_t _spawn_task(fpr_node_t *net, void (*entry)(void *),
                             const char *name, const fpr_task_slot_t *slot,
                             TaskHandle_t *handle, volatile bool *stop_flag)
{
    if (slot == NULL || slot->stack == NULL || slot->tcb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *stop_flag = false;
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        entry, name, slot->stack_bytes, net, slot->priority,
        slot->stack, slot->tcb, slot->core);
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *handle = h;
    taskYIELD();
    return ESP_OK;
}

esp_err_t fpr_start_loop_task(fpr_node_t *net, uint32_t duration_ms,
                              const fpr_task_slot_t *slot, bool force_restart)
{
    if (net->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (net->loop_task != NULL && !force_restart) {
        return ESP_ERR_INVALID_STATE;
    }
    if (net->loop_task != NULL) {
        _fpr_task_stop(net, &net->loop_task, &net->loop_stop_requested);
    }
    esp_err_t err = fpr_start_loop(net, duration_ms);
    if (err != ESP_OK) {
        return err;
    }
    err = _spawn_task(net, _fpr_loop_task_entry, "FPR_Loop", slot,
                      &net->loop_task, &net->loop_stop_requested);
    if (err != ESP_OK) {
        net->loop_armed = false;
    }
    return err;
}

esp_err_t fpr_stop_loop_task(fpr_node_t *net)
{
    if (net->loop_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    net->loop_armed = false;
    _fpr_task_stop(net, &net->loop_task, &net->loop_stop_requested);
    return ESP_OK;
}

bool fpr_loop_task_running(fpr_node_t *net)
{
    return net->loop_task != NULL;
}

esp_err_t fpr_start_reconnect_task(fpr_node_t *net, const fpr_task_slot_t *slot)
{
    if (net->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (net->reconnect_task != NULL) {
        ESP_LOGW(TAG, "Reconnect task already running");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = fpr_start_reconnect(net);
    if (err != ESP_OK) {
        return err;
    }
    err = _spawn_task(net, _fpr_reconnect_task_entry, "FPR_Reconnect", slot,
                      &net->reconnect_task, &net->reconnect_stop_requested);
    if (err != ESP_OK) {
        net->reconnect_armed = false;
    }
    return err;
}

esp_err_t fpr_stop_reconnect_task(fpr_node_t *net)
{
    /* Only stops the task — protocol handlers and network state are
     * intentionally untouched. */
    if (net->reconnect_task == NULL) {
        return ESP_OK;
    }
    net->reconnect_armed = false;
    _fpr_task_stop(net, &net->reconnect_task, &net->reconnect_stop_requested);
    return ESP_OK;
}

bool fpr_reconnect_task_running(fpr_node_t *net)
{
    return net->reconnect_task != NULL;
}

// ========== NETWORK STATE MANAGEMENT ==========

esp_err_t fpr_pause(fpr_node_t *net)
{
    if (net->state != FPR_STATE_STARTED) {
        ESP_LOGW(TAG, "Network not started, cannot pause");
        return ESP_ERR_INVALID_STATE;
    }
    
    _peers_lock(net);
    net->paused = true;
    net->state = FPR_STATE_PAUSED;
    _peers_unlock(net);
    ESP_LOGI(TAG, "Network paused");
    return ESP_OK;
}

esp_err_t fpr_resume(fpr_node_t *net)
{
    if (net->state != FPR_STATE_PAUSED) {
        ESP_LOGW(TAG, "Network not paused, cannot resume");
        return ESP_ERR_INVALID_STATE;
    }
    
    _peers_lock(net);
    net->paused = false;
    net->state = FPR_STATE_STARTED;
    _peers_unlock(net);
    ESP_LOGI(TAG, "Network resumed");
    return ESP_OK;
}

fpr_state_t fpr_get_state(fpr_node_t *net)
{
    return net->state;
}

void fpr_set_power_mode(fpr_node_t *net, fpr_power_mode_t mode)
{
    _peers_lock(net);
    net->power_mode = mode;
    _peers_unlock(net);
    ESP_LOGI(TAG, "Power mode set to %s", mode == FPR_POWER_LOW ? "LOW" : "NORMAL");
}

fpr_power_mode_t fpr_get_power_mode(fpr_node_t *net)
{
    return net->power_mode;
}

uint8_t fpr_get_channel(fpr_node_t *net)
{
    return net->channel;
}

// ========== QUEUE MODE API ==========

void fpr_set_queue_mode(fpr_node_t *net, fpr_queue_mode_t mode)
{
    _peers_lock(net);
    net->default_queue_mode = mode;
    _peers_unlock(net);
    ESP_LOGI(TAG, "Default queue mode set to %s",
             mode == FPR_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
}

void fpr_set_visibility(fpr_node_t *net, fpr_visibility_t state)
{
    _peers_lock(net);
    net->access_state = state;
    _peers_unlock(net);
}

fpr_visibility_t fpr_get_visibility(fpr_node_t *net)
{
    _peers_lock(net);
    fpr_visibility_t state = net->access_state;
    _peers_unlock(net);
    return state;
}

fpr_queue_mode_t fpr_get_queue_mode(fpr_node_t *net)
{
    return net->default_queue_mode;
}

