/**
 * @file fpr_peers.c
 * @brief FPR Peer Management: add/remove, queries, enumeration, per-peer queues
 */
#include "fpr/fpr.h"
#include "fpr/internal/helpers.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "fpr_peers";
esp_err_t fpr_network_add_peer(const uint8_t *peer_mac)
{
    return _peer_add(peer_mac, NULL, false, 0);
}

esp_err_t fpr_network_remove_peer(const uint8_t *peer_mac)
{
    return _peer_remove(peer_mac);
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

typedef struct {
    FPR_STORE_HASH_TYPE **stores;
    size_t cap;
    size_t n;
} _peer_snapshot_ctx_t;

/* Collect (and reference) every peer store under the lock so the user
 * callback can run unlocked — hashmap_foreach only tolerates removal
 * of the entry currently being visited, but a user callback may remove
 * any peer or clear the map outright. */
static void _peer_snapshot_cb(void *key, void *value, void *user_data)
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
    hashmap_foreach(&fpr_net.peers_map, _peer_snapshot_cb, &ctx);
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

esp_err_t fpr_get_peer_info(const uint8_t *peer_mac, fpr_peer_info_t *info)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && info != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    
    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGD(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }
    _peer_copy_to_info(peer, info);
    _peers_unlock();
    return ESP_OK;
}

typedef struct {
    fpr_peer_info_t *array;
    size_t max_peers;
    size_t count;
} peer_list_ctx_t;

static void _peer_list_cb(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    peer_list_ctx_t *ctx = (peer_list_ctx_t *)user_data;
    
    if (ctx->count < ctx->max_peers && peer) {
        _peer_copy_to_info(peer, &ctx->array[ctx->count]);
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
    hashmap_foreach(&fpr_net.peers_map, _peer_list_cb, &ctx);
    _peers_unlock();
    return ctx.count;
}

bool fpr_network_get_data_from_peer(const uint8_t *peer_mac, void *data, int data_size, TickType_t timeout)
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
                    memcpy(data, &pkg.protocol, copy_size);
                    // Decrement queued packet count (complete packet consumed)
                    _peer_dec_queued(peer);
                    result = true;
                    goto out;  // Success - got complete single packet

                case FPR_PACKAGE_TYPE_START:
                    // Begin a multi-packet transfer - reset offset; a START
                    // arriving mid-read restarts the message, so recompute
                    // the bound against the full buffer.
                    offset = 0;
                    expecting_more = true;
                    copy_size = ((size_t)data_size < actual_payload) ? (size_t)data_size : actual_payload;
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

esp_err_t fpr_network_set_peer_queue_mode(const uint8_t *peer_mac, fpr_queue_mode_t mode)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
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

uint32_t fpr_network_get_peer_queued_packets(const uint8_t *peer_mac)
{
    if (peer_mac == NULL) {
        return 0;
    }

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    uint32_t count = peer ? peer->queued_packets : 0;
    _peers_unlock();
    return count;
}

typedef struct {
    const char *name;
    uint8_t *mac_out;
    bool found;
} peer_name_search_ctx_t;

static void _peer_by_name_cb(void *key, void *value, void *user_data)
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
    hashmap_foreach(&fpr_net.peers_map, _peer_by_name_cb, &ctx);
    _peers_unlock();

    if (!ctx.found) {
        return ESP_ERR_NOT_FOUND;
    }
    
    return ESP_OK;
}

static void _peer_clear_cb(void *key, void *value, void *user_data)
{
    (void)user_data;
    const uint8_t *mac = (const uint8_t *)key;
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
    hashmap_foreach(&fpr_net.peers_map, _peer_clear_cb, NULL);

    // Clear the hashmap
    hashmap_clear(&fpr_net.peers_map);

    // Extender routes are useless without the peers they describe
    hashmap_foreach(&fpr_net.routes_map, _route_free_cb, NULL);
    hashmap_clear(&fpr_net.routes_map);
    _peers_unlock();

    ESP_LOGI(TAG, "Cleared %zu peers", peer_count);
    return ESP_OK;
}

bool fpr_is_peer_reachable(const uint8_t *peer_mac, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, false, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGW(TAG, "Peer not found in peer map");
        return false;
    }

    // Check if peer was recently seen (within timeout)
    int64_t now_us = esp_timer_get_time();
    int64_t age_us = now_us - peer->last_seen;
    uint64_t age_ms = (uint64_t)FPR_US_TO_MS(age_us);
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
        peer = _peer_from_map(peer_mac);
        bool responded = (peer && peer->last_seen > initial_last_seen);
        _peers_unlock();
        if (responded) {
            return true;  // Peer responded
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // Small delay between checks
    }

    return false;  // Timeout - peer did not respond
}