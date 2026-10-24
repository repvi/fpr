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

/* Public-API node guard: a NULL node must never reach _peers_lock or a
 * field dereference. ESP_RETURN_ON_FALSE logs + returns INVALID_ARG. */
#define FPR_REQUIRE_NODE(n) \
    ESP_RETURN_ON_FALSE((n) != NULL, ESP_ERR_INVALID_ARG, TAG, "Node is NULL")

static void _rx_dispatch(void *sink, const fpr_rx_frame_t *frame);
static void _tx_done_sink(void *sink, const uint8_t *mac, bool ok);

/* Cooperative-stop grace for the loop/reconnect tasks: they poll their
 * stop flag once per configured poll interval (power-mode stretched),
 * so the window must cover one worst-case iteration plus lock hold. */
static inline TickType_t _task_stop_grace(fpr_node_t *node)
{
    uint32_t interval =
        _power_interval(node, FPR_HOST_SCAN_POLL_INTERVAL_MS);
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
static void _fpr_task_stop(fpr_node_t *node, TaskHandle_t *handle, volatile bool *stop_flag)
{
    if (handle == NULL || *handle == NULL) {
        return;
    }
    TaskHandle_t target = *handle;
    *stop_flag = true;
    TickType_t deadline = xTaskGetTickCount() + _task_stop_grace(node);
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
static void _task_exit(fpr_node_t *node, TaskHandle_t *slot)
{
    _peers_lock(node);
    if (*slot == xTaskGetCurrentTaskHandle()) {
        *slot = NULL;
    }
    _peers_unlock(node);
    vTaskDelete(NULL);
}

// Helper: Add broadcast peer to the transport's peer table
// this needs to be called whenever we change mode (host/client/extender)
static esp_err_t _broadcast_peer_add(fpr_node_t *node, const char *mode_name)
{
    static const uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    const fpr_transport_t *t = _transport(node);
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
esp_err_t fpr_init_at(fpr_node_t *node, const char *name, const fpr_init_config_t *config)
{
    ESP_RETURN_ON_FALSE(node != NULL, ESP_ERR_INVALID_ARG, TAG, "Node is NULL");
    ESP_RETURN_ON_FALSE(name != NULL, ESP_ERR_INVALID_ARG, TAG, "Name is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    ESP_RETURN_ON_FALSE(strlen(name) < sizeof(node->name), ESP_ERR_INVALID_ARG, TAG, "Name too long");
    ESP_RETURN_ON_FALSE(node->state == FPR_STATE_UNINITIALIZED, ESP_ERR_INVALID_STATE, TAG, "Network already initialized");

    node->transport = config->transport ? config->transport
                                          : fpr_transport_espnow();
    const fpr_transport_t *t = node->transport;
    /* Required vtable members — a NULL fn pointer here is a delayed
     * crash, so reject the transport up front. set_channel is the one
     * optional op (skipped when absent or channel unset). */
    ESP_RETURN_ON_FALSE(t->open && t->close && t->send && t->get_mac &&
                        t->ready && t->add_peer && t->del_peer,
                        ESP_ERR_INVALID_ARG, TAG,
                        "Transport vtable missing required ops");
    ESP_RETURN_ON_FALSE(config->channel <= 14, ESP_ERR_INVALID_ARG,
                        TAG, "Invalid WiFi channel %d", config->channel);
    ESP_RETURN_ON_FALSE(t->mtu > sizeof(fpr_package_t), ESP_ERR_INVALID_SIZE,
                        TAG, "Transport MTU %u too small for fpr_package_t (%u)",
                        (unsigned)t->mtu, (unsigned)sizeof(fpr_package_t));
    ESP_RETURN_ON_ERROR(t->get_mac(t->ctx, node->mac), TAG, "Failed to read MAC address");
    
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = '\0';
    // Store channel and power mode config
    node->channel = config->channel;
    node->power_mode = config->power_mode;
    node->default_queue_mode = config->queue_mode;

    node->access_state = FPR_VISIBILITY_PUBLIC;
    
    // Initialize default host config
    node->host_config.max_peers = 32;  // 32 by default
    node->host_config.connection_mode = FPR_CONNECTION_AUTO;
    node->host_config.request_cb = NULL;
    
    // Initialize default client config
    node->client_config.connection_mode = FPR_CONNECTION_AUTO;
    node->client_config.discovery_cb = NULL;

    // Initialize security
    node->host_pwk_valid = false;
    
    // Initialize sequence number counter
    node->tx_sequence_num = 0;

    /* peers_lock is process-lifetime: created lazily here and never
     * deleted. A callback or reader may legitimately reach for it after
     * deinit (radio callbacks already dispatched, a blocked
     * get_data_from_peer waking late) — deleting the semaphore under a
     * waiter is UB, and the guarded objects are left in hashmap's
     * safe-empty state anyway. */
    if (node->peers_lock == NULL) {
        node->peers_lock = xSemaphoreCreateRecursiveMutex();
        if (node->peers_lock == NULL) {
            ESP_LOGE(TAG, "Failed to allocate peers lock");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!hashmap_init(&node->peers_map, FPR_HASHMAP_INITIAL_SIZE, mac_hash, mac_equals)) {
        ESP_LOGE(TAG, "Failed to allocate peers map");
        return ESP_ERR_NO_MEM;
    }
    if (!hashmap_init(&node->routes_map, FPR_HASHMAP_INITIAL_SIZE, mac_hash, mac_equals)) {
        ESP_LOGE(TAG, "Failed to allocate routes map");
        hashmap_free(&node->peers_map);
        return ESP_ERR_NO_MEM;
    }
    /* Bring the radio up with the dispatch sinks installed. RX frames
     * arriving before set_mode installs a handler are dropped, matching
     * the old "no recv cb registered until start" behaviour. */
    esp_err_t radio_err = t->open(t->ctx, _rx_dispatch, _tx_done_sink, node);
    if (radio_err != ESP_OK) {
        /* The maps are already allocated — free them or every init retry
         * on a degraded radio leaks the bucket arrays. */
        hashmap_free(&node->routes_map);
        hashmap_free(&node->peers_map);
        ESP_LOGE(TAG, "Failed to open transport: %s", esp_err_to_name(radio_err));
        return radio_err;
    }

    /* Channel select and the broadcast peer need a live radio: the
     * transport contract is that every vtable op fails while closed,
     * so both run only after open() succeeds. The broadcast peer is
     * also re-added by fpr_set_mode for client/host/extender — the
     * init-time add covers modes that never take a branch there
     * (BROADCAST/DEFAULT). */
    if (config->channel >= 1 && t->set_channel) {
        esp_err_t err = t->set_channel(t->ctx, config->channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi channel %d: %s", config->channel, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi channel set to %d", config->channel);
        }
    }
    if (_broadcast_peer_add(node, "default") != ESP_OK) {
        ESP_LOGW(TAG, "Broadcast peer add failed at init");
    }

    node->state = FPR_STATE_INITIALIZED;
    node->paused = false;
    
    ESP_LOGI(TAG, "FPR Network initialized: %s (" MACSTR ") ch=%d pwr=%s queue=%s", 
             node->name, MAC2STR(node->mac),
             node->channel ? node->channel : 0,
             node->power_mode == FPR_POWER_LOW ? "LOW" : "NORMAL",
             node->default_queue_mode == FPR_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
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

static void _reset_all_peers(fpr_node_t *node)
{
    hashmap_foreach(&node->peers_map, _peer_cleanup_cb, node);
}

esp_err_t fpr_deinit(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    /* Quiesce gate FIRST — peer adds and extender route learning reject
     * immediately; a callback still in flight sees UNINITIALIZED when it
     * re-checks state under the lock, before any teardown below runs. */
    _peers_lock(node);
    node->state = FPR_STATE_UNINITIALIZED;
    _peers_unlock(node);

    /* Ask the internal tasks to exit rather than vTaskDelete them: a
     * task killed while holding peers_lock strands the recursive mutex
     * for every later caller (FreeRTOS never releases a dead task's
     * mutexes). The helper joins on the task clearing its own handle
     * slot inside a poll-interval grace window. */
    node->loop_armed = false;
    node->reconnect_armed = false;
    _fpr_task_stop(node, &node->reconnect_task, &node->reconnect_stop_requested);
    _fpr_task_stop(node, &node->loop_task, &node->loop_stop_requested);

    /* close() unregisters the recv sink before tearing the radio down:
     * nothing new is dispatched on the wifi task after it returns, so
     * only callbacks already in flight remain below. A deinit before
     * any successful init has no transport yet — nothing to close. */
    esp_err_t radio_result = node->transport
        ? node->transport->close(node->transport->ctx) : ESP_OK;

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
    _peers_lock(node);
    _reset_all_peers(node);
    hashmap_free(&node->peers_map);
    hashmap_foreach(&node->routes_map, _route_free_cb, NULL);
    hashmap_free(&node->routes_map);
    _peers_unlock(node);

    /* peers_lock survives deinit: a straggler callback or a blocked
     * get_data_from_peer reader may still reach for it after the memset
     * — deleting the semaphore under a waiter would be use-after-free.
     * For heap nodes (owns_storage) fpr_destroy frees it once the
     * caller guarantees callbacks have drained; caller-provided
     * (static) nodes keep the lock alive — the storage owner decides. */
    SemaphoreHandle_t kept_lock = node->peers_lock;
    bool owned = node->owns_storage;
    memset(node, 0, sizeof(*node));
    node->state = FPR_STATE_UNINITIALIZED;
    node->peers_lock = kept_lock;
    node->owns_storage = owned;

    return radio_result;
}

/* Allocate + init a node in one call — the public entry point. */
esp_err_t fpr_create(const fpr_node_config_t *cfg, fpr_node_t **out)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && out != NULL, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    fpr_node_t *node = heap_caps_calloc(1, sizeof(*node), MALLOC_CAP_8BIT);
    if (node == NULL) {
        return ESP_ERR_NO_MEM;
    }
    node->owns_storage = true;
    node->callbacks = cfg->callbacks;
    esp_err_t err = fpr_init_at(node, cfg->name, &cfg->init);
    if (err != ESP_OK) {
        if (node->peers_lock != NULL) {
            vSemaphoreDelete(node->peers_lock);
        }
        heap_caps_free(node);
        return err;
    }
    *out = node;
    return ESP_OK;
}

void fpr_destroy(fpr_node_t *node)
{
    if (node == NULL) {
        return;
    }
    /* Legal only after fpr_deinit(): the transport close there has
     * drained every callback, so freeing the lock is now safe for an
     * owned node. */
    if (node->state != FPR_STATE_UNINITIALIZED) {
        fpr_deinit(node);
    }
    if (!node->owns_storage) {
        /* Caller-provided storage (fpr_init_at): the node is left
         * deinitialized and peers_lock stays alive — freeing either is
         * the storage owner's call. */
        return;
    }
    if (node->peers_lock != NULL) {
        vSemaphoreDelete(node->peers_lock);
    }
    heap_caps_free(node);
}

/* Runtime callback table swap — the registered pointers may fire from
 * the transport RX context, so callers must keep them valid until
 * fpr_stop()/fpr_deinit() drains it. */
esp_err_t fpr_set_callbacks(fpr_node_t *node, const fpr_node_callbacks_t *callbacks)
{
    FPR_REQUIRE_NODE(node);
    _peers_lock(node);
    if (callbacks != NULL) {
        node->callbacks = *callbacks;
    } else {
        node->callbacks = (fpr_node_callbacks_t){0};
    }
    _peers_unlock(node);
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
    fpr_node_t *node = (fpr_node_t *)sink;
    /* A frame we cannot address cannot be routed: src/dst MAC are
     * required by every mode handler. */
    if (node && frame && frame->data && frame->src_mac && frame->dst_mac &&
        node->rx_handler) {
        node->rx_handler(node, frame, frame->data, (int)frame->len);
    }
}

esp_err_t fpr_start(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(node->state == FPR_STATE_INITIALIZED || node->state == FPR_STATE_STOPPED,
                        ESP_ERR_INVALID_STATE, TAG, "Network not initialized (state %d)", node->state);
    ESP_RETURN_ON_FALSE(node->transport->ready(node->transport->ctx),
                        ESP_ERR_INVALID_STATE, TAG, "Radio is not initialized or in a usable mode");

    /* A node that never picked a mode comes up as a CLIENT — modeless
     * started nodes have no RX handler and would silently drop frames.
     * An explicit fpr_set_mode() before start is honored. */
    if (node->current_mode == FPR_MODE_DEFAULT) {
        fpr_set_mode(node, FPR_MODE_CLIENT);
    }

    _peers_lock(node);
    node->state = FPR_STATE_STARTED;
    node->paused = false;
    _peers_unlock(node);

    ESP_LOGI(TAG, "FPR Network started with MAC: " MACSTR, MAC2STR(node->mac));
    return ESP_OK;
}

esp_err_t fpr_stop(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    if (node->state == FPR_STATE_STOPPED || node->state == FPR_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Network already stopped or not initialized");
        return ESP_OK;
    }
    
    _peers_lock(node);
    node->state = FPR_STATE_STOPPED;
    node->paused = false;
    /* Stop means no periodic work: a stopped node must not keep
     * broadcasting or reconnecting via fpr_poll / the node tasks. */
    node->loop_armed = false;
    node->reconnect_armed = false;
    _peers_unlock(node);
    ESP_LOGI(TAG, "Network stopped");
    return ESP_OK;
}

/* Install the mode's protocol handler for _rx_dispatch. An assignment
 * now — the transport sink is fixed — kept as a function so set_mode
 * stays readable. */
/* Install the mode's protocol handler for _rx_dispatch. An assignment
 * — the transport sink is fixed — kept as a function so set_mode
 * stays readable. */
static void _set_rx_handler(fpr_node_t *node,
                            void (*handler)(fpr_node_t *, const fpr_rx_frame_t *,
                                            const uint8_t *, int))
{
    node->rx_handler = handler;
}

void _client_loop_step(fpr_node_t *node)
{
    /* The RX handler does all the work; the loop interval only paces it. */
    (void)node;
}

void _host_loop_step(fpr_node_t *node)
{
    /* Announce presence — the RX handler does the connection work. */
    fpr_broadcast_device_info(node);
}

static void _loop_step(fpr_node_t *node)
{
    if (node->current_mode == FPR_MODE_HOST) {
        _host_loop_step(node);
    } else {
        _client_loop_step(node);
    }
}

/* Shared pacing for both periodic halves — power-mode stretched. */
static uint32_t _step_interval_ms(fpr_node_t *node)
{
    return _power_interval(node, node->current_mode == FPR_MODE_HOST
                                  ? FPR_HOST_SCAN_POLL_INTERVAL_MS
                                  : FPR_CLIENT_WAIT_CHECK_INTERVAL_MS);
}

static void _reconnect_step(fpr_node_t *node, uint32_t now_ms)
{
    if (node->current_mode == FPR_MODE_HOST) {
        _host_reconnect_step(node, now_ms);
    } else {
        _client_reconnect_step(node, now_ms);
    }
}

static uint32_t _poll_loop(fpr_node_t *node, uint32_t now_ms);
static uint32_t _poll_reconnect(fpr_node_t *node, uint32_t now_ms);

/* Loop task body: arg is the node. Exit when disarmed (duration bound
 * reached) or stop-requested — poll() disarms on the deadline. */
static void _fpr_loop_task_entry(void *arg)
{
    fpr_node_t *node = (fpr_node_t *)arg;
    ESP_LOGI(TAG, "%s loop task started",
             node->current_mode == FPR_MODE_HOST ? "Host" : "Client");

    while (!node->loop_stop_requested && node->loop_armed) {
        uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t wait_ms = _poll_loop(node, now_ms);
        if (wait_ms == 0 || wait_ms == UINT32_MAX) {
            wait_ms = _step_interval_ms(node);
        }
        /* wait_ms is relative to now, not last_wake — vTaskDelayUntil
         * would wake early by (now - last_wake) and spin re-polls. */
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    _task_exit(node, &node->loop_task);
}

/* Same pattern for the reconnect/keepalive task — runs only the
 * reconnect half of the poll so the two tasks never double-step. */
static void _fpr_reconnect_task_entry(void *arg)
{
    fpr_node_t *node = (fpr_node_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (!node->reconnect_stop_requested && node->reconnect_armed) {
        uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        _poll_reconnect(node, now_ms);
        /* vTaskDelayUntil stays here on purpose: this task paces a fixed
         * interval (not a poll-returned deadline relative to now), which
         * is exactly the periodic-wakeup primitive's contract. */
        vTaskDelayUntil(&last_wake,
                        pdMS_TO_TICKS(_step_interval_ms(node)));
    }

    _task_exit(node, &node->reconnect_task);
}

esp_err_t fpr_set_mode(fpr_node_t *node, fpr_mode_type_t mode)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(mode >= FPR_MODE_DEFAULT && mode <= FPR_MODE_EXTENDER,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid mode %d", (int)mode);
    node->current_mode = mode;
    /* Mesh forwarding only runs in extender mode. */
    node->routing_enabled = (mode == FPR_MODE_EXTENDER);

    /* Client/host/extender each need the broadcast peer registered and
     * the mode's RX handler installed. The mode itself is applied even
     * when a step fails; the first failure is reported to the caller. */
    esp_err_t first_err = ESP_OK;
    if (mode == FPR_MODE_CLIENT) {
        first_err = _broadcast_peer_add(node, "client");
        if (first_err != ESP_OK) {
            ESP_LOGW(TAG, "Broadcast peer add failed for client mode: %s",
                     esp_err_to_name(first_err));
        }
        _set_rx_handler(node, _rx_client);
    }
    else if (mode == FPR_MODE_HOST) {
        first_err = _broadcast_peer_add(node, "host");
        if (first_err != ESP_OK) {
            ESP_LOGW(TAG, "Broadcast peer add failed for host mode: %s",
                     esp_err_to_name(first_err));
        }
        // Generate host PWK for secure connections — under the lock so
        // rx callbacks never observe a valid flag ahead of the key bytes
        _peers_lock(node);
        esp_err_t pwk_err = fpr_security_generate_pwk(node->host_pwk);
        if (pwk_err == ESP_OK) {
            node->host_pwk_valid = true;
            ESP_LOGI(TAG, "Host mode set with generated PWK");
        } else {
            ESP_LOGE(TAG, "Failed to generate PWK for host mode: %s",
                     esp_err_to_name(pwk_err));
            if (first_err == ESP_OK) {
                first_err = pwk_err;
            }
        }
        _peers_unlock(node);
        _set_rx_handler(node, _rx_host);
    }
    else if (mode == FPR_MODE_EXTENDER) {
        first_err = _broadcast_peer_add(node, "extender");
        if (first_err != ESP_OK) {
            ESP_LOGW(TAG, "Broadcast peer add failed for extender mode: %s",
                     esp_err_to_name(first_err));
        }
        _set_rx_handler(node, _rx_extender);
    }
    else {
        /* FPR_MODE_DEFAULT/FPR_MODE_BROADCAST: accepted but have no
         * handler of their own — the previous mode's RX stays armed. */
        ESP_LOGW(TAG, "Mode %d has no handler — RX callback unchanged", (int)mode);
    }
    return first_err;
}

esp_err_t fpr_get_mode(fpr_node_t *node, fpr_mode_type_t *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = node->current_mode;
    return ESP_OK;
}

// ========== PERIODIC WORK: poll + task control ==========

esp_err_t fpr_start_loop(fpr_node_t *node, uint32_t duration_ms)
{
    FPR_REQUIRE_NODE(node);
    if (node->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (node->current_mode == FPR_MODE_EXTENDER) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (node->current_mode != FPR_MODE_CLIENT && node->current_mode != FPR_MODE_HOST) {
        return ESP_ERR_INVALID_STATE;
    }
    node->loop_armed = true;
    /* Due at the next poll call; the deadline is anchored against the
     * poll clock on the first run (duration_ms == 0 means run forever). */
    node->loop_next_due_ms = 0;
    node->loop_duration_ms = duration_ms;
    node->loop_deadline_ms = 0;
    return ESP_OK;
}

esp_err_t fpr_stop_loop(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    node->loop_armed = false;
    return ESP_OK;
}

esp_err_t fpr_start_reconnect(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    if (node->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (node->current_mode != FPR_MODE_CLIENT && node->current_mode != FPR_MODE_HOST) {
        return ESP_ERR_INVALID_STATE;
    }
    node->reconnect_armed = true;
    node->reconnect_next_due_ms = 0;
    return ESP_OK;
}

esp_err_t fpr_stop_reconnect(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    node->reconnect_armed = false;
    return ESP_OK;
}

static uint32_t _poll_loop(fpr_node_t *node, uint32_t now_ms)
{
    uint32_t next = UINT32_MAX;

    if (node->loop_armed) {
        if (node->loop_deadline_ms == 0 && node->loop_duration_ms) {
            node->loop_deadline_ms = now_ms + node->loop_duration_ms;
        }
        if (node->loop_deadline_ms && (int32_t)(now_ms - node->loop_deadline_ms) >= 0) {
            node->loop_armed = false;
        /* next_due == 0 is the "armed, due at next poll" sentinel from
         * fpr_start_loop — comparing it as a timestamp would read as
         * far-future once now_ms passes 2^31. A computed due can only
         * hit 0 once per epoch wrap, costing at most one early step. */
        } else if (node->loop_next_due_ms == 0 ||
                   (int32_t)(now_ms - node->loop_next_due_ms) >= 0) {
            _loop_step(node);
            node->loop_next_due_ms = now_ms + _step_interval_ms(node);
        }
        if (node->loop_armed) {
            uint32_t due = node->loop_next_due_ms - now_ms;
            if (node->loop_deadline_ms) {
                uint32_t dl = node->loop_deadline_ms - now_ms;
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

static uint32_t _poll_reconnect(fpr_node_t *node, uint32_t now_ms)
{
    uint32_t next = UINT32_MAX;

    if (node->reconnect_armed) {
        /* Same 0 sentinel as the loop half (see _poll_loop). */
        if (node->reconnect_next_due_ms == 0 ||
            (int32_t)(now_ms - node->reconnect_next_due_ms) >= 0) {
            _reconnect_step(node, now_ms);
            node->reconnect_next_due_ms = now_ms + _step_interval_ms(node);
        }
        uint32_t due = node->reconnect_next_due_ms - now_ms;
        if (due < next) {
            next = due;
        }
    }

    return next;
}

esp_err_t fpr_poll(fpr_node_t *node, uint32_t now_ms, uint32_t *next_due_ms_out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(next_due_ms_out != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Output is NULL");
    uint32_t l = _poll_loop(node, now_ms);
    uint32_t r = _poll_reconnect(node, now_ms);
    *next_due_ms_out = l < r ? l : r;
    return ESP_OK;
}

/* Spawn one periodic task on caller-owned static storage. The handle
 * lands in node->loop_task / node->reconnect_task so the cooperative
 * stop path is shared with fpr_stop(). */
static esp_err_t _spawn_task(fpr_node_t *node, void (*entry)(void *),
                             const char *name, const fpr_task_slot_t *slot,
                             TaskHandle_t *handle, volatile bool *stop_flag)
{
    if (slot == NULL || slot->stack == NULL || slot->tcb == NULL ||
        slot->stack_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *stop_flag = false;
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        entry, name, slot->stack_bytes, node, slot->priority,
        slot->stack, slot->tcb, slot->core);
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *handle = h;
    taskYIELD();
    return ESP_OK;
}

esp_err_t fpr_start_loop_task(fpr_node_t *node, uint32_t duration_ms,
                              const fpr_task_slot_t *slot, bool force_restart)
{
    FPR_REQUIRE_NODE(node);
    if (node->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (node->loop_task != NULL && !force_restart) {
        return ESP_ERR_INVALID_STATE;
    }
    if (node->loop_task != NULL) {
        _fpr_task_stop(node, &node->loop_task, &node->loop_stop_requested);
    }
    esp_err_t err = fpr_start_loop(node, duration_ms);
    if (err != ESP_OK) {
        return err;
    }
    err = _spawn_task(node, _fpr_loop_task_entry, "FPR_Loop", slot,
                      &node->loop_task, &node->loop_stop_requested);
    if (err != ESP_OK) {
        node->loop_armed = false;
    }
    return err;
}

esp_err_t fpr_stop_loop_task(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    if (node->loop_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    node->loop_armed = false;
    _fpr_task_stop(node, &node->loop_task, &node->loop_stop_requested);
    return ESP_OK;
}

esp_err_t fpr_loop_task_running(fpr_node_t *node, bool *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = node->loop_task != NULL;
    return ESP_OK;
}

esp_err_t fpr_start_reconnect_task(fpr_node_t *node, const fpr_task_slot_t *slot)
{
    FPR_REQUIRE_NODE(node);
    if (node->state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (node->reconnect_task != NULL) {
        ESP_LOGW(TAG, "Reconnect task already running");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = fpr_start_reconnect(node);
    if (err != ESP_OK) {
        return err;
    }
    err = _spawn_task(node, _fpr_reconnect_task_entry, "FPR_Reconnect", slot,
                      &node->reconnect_task, &node->reconnect_stop_requested);
    if (err != ESP_OK) {
        node->reconnect_armed = false;
    }
    return err;
}

esp_err_t fpr_stop_reconnect_task(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    /* Only stops the task — protocol handlers and network state are
     * intentionally untouched. */
    if (node->reconnect_task == NULL) {
        return ESP_OK;
    }
    node->reconnect_armed = false;
    _fpr_task_stop(node, &node->reconnect_task, &node->reconnect_stop_requested);
    return ESP_OK;
}

esp_err_t fpr_reconnect_task_running(fpr_node_t *node, bool *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = node->reconnect_task != NULL;
    return ESP_OK;
}

// ========== NETWORK STATE MANAGEMENT ==========

esp_err_t fpr_pause(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    if (node->state != FPR_STATE_STARTED) {
        ESP_LOGW(TAG, "Network not started, cannot pause");
        return ESP_ERR_INVALID_STATE;
    }
    
    _peers_lock(node);
    node->paused = true;
    node->state = FPR_STATE_PAUSED;
    _peers_unlock(node);
    ESP_LOGI(TAG, "Network paused");
    return ESP_OK;
}

esp_err_t fpr_resume(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    if (node->state != FPR_STATE_PAUSED) {
        ESP_LOGW(TAG, "Network not paused, cannot resume");
        return ESP_ERR_INVALID_STATE;
    }
    
    _peers_lock(node);
    node->paused = false;
    node->state = FPR_STATE_STARTED;
    _peers_unlock(node);
    ESP_LOGI(TAG, "Network resumed");
    return ESP_OK;
}

esp_err_t fpr_get_state(fpr_node_t *node, fpr_state_t *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = node->state;
    return ESP_OK;
}

esp_err_t fpr_set_power_mode(fpr_node_t *node, fpr_power_mode_t mode)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(mode == FPR_POWER_NORMAL || mode == FPR_POWER_LOW,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid power mode %d", (int)mode);
    _peers_lock(node);
    node->power_mode = mode;
    _peers_unlock(node);
    ESP_LOGI(TAG, "Power mode set to %s", mode == FPR_POWER_LOW ? "LOW" : "NORMAL");
    return ESP_OK;
}

esp_err_t fpr_get_power_mode(fpr_node_t *node, fpr_power_mode_t *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = node->power_mode;
    return ESP_OK;
}

esp_err_t fpr_get_channel(fpr_node_t *node, uint8_t *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = node->channel;
    return ESP_OK;
}

// ========== QUEUE MODE API ==========

esp_err_t fpr_set_queue_mode(fpr_node_t *node, fpr_queue_mode_t mode)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(mode == FPR_QUEUE_MODE_NORMAL ||
                        mode == FPR_QUEUE_MODE_LATEST_ONLY,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid queue mode %d", (int)mode);
    _peers_lock(node);
    node->default_queue_mode = mode;
    _peers_unlock(node);
    ESP_LOGI(TAG, "Default queue mode set to %s",
             mode == FPR_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
    return ESP_OK;
}

esp_err_t fpr_set_visibility(fpr_node_t *node, fpr_visibility_t state)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(state == FPR_VISIBILITY_PUBLIC ||
                        state == FPR_VISIBILITY_PRIVATE,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid visibility %d", (int)state);
    _peers_lock(node);
    node->access_state = state;
    _peers_unlock(node);
    return ESP_OK;
}

esp_err_t fpr_get_visibility(fpr_node_t *node, fpr_visibility_t *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    _peers_lock(node);
    *out = node->access_state;
    _peers_unlock(node);
    return ESP_OK;
}

esp_err_t fpr_get_queue_mode(fpr_node_t *node, fpr_queue_mode_t *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = node->default_queue_mode;
    return ESP_OK;
}

