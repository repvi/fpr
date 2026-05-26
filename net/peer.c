/**
 * @file peer.c
 * @brief Sprout Peer Management APIs
 *
 * Peer addition, removal, lookup, listing, and route table management.
 */

#include "sprout/internal/helpers.h"
#include "sprout/sprout.h"
#include "sprout/internal/private_defs.h"
#include "sprout/sprout_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "string.h"

static const char *TAG = "sprout_peer";

extern sprout_network_t sprout_net;

esp_err_t sprout_add_peer(uint8_t *peer_mac)
{
    return _add_peer_internal(peer_mac, NULL, false, 0);
}

esp_err_t sprout_remove_peer(uint8_t *peer_mac)
{
    return _remove_peer_internal(peer_mac);
}

int sprout_get_peer_count(void)
{
    return hashmap_size(sprout_net.peers_map);
}

esp_err_t sprout_get_peer_info(uint8_t *peer_mac, sprout_peer_info_t *info)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && info != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");

    _copy_peer_to_info(peer, info);
    return ESP_OK;
}

typedef struct {
    sprout_peer_info_t *array;
    size_t max_peers;
    size_t count;
} peer_list_ctx_t;

static void _list_peers_callback(void *key, void *value, void *user_data)
{
    (void)key;
    SPROUT_STORE_HASH_TYPE *peer = (SPROUT_STORE_HASH_TYPE *)value;
    peer_list_ctx_t *ctx = (peer_list_ctx_t *)user_data;

    if (ctx->count < ctx->max_peers && peer) {
        _copy_peer_to_info(peer, &ctx->array[ctx->count]);
        ctx->count++;
    }
}

size_t sprout_list_all_peers(sprout_peer_info_t *peer_array, size_t max_peers)
{
    if (!peer_array || max_peers == 0) {
        return 0;
    }

    peer_list_ctx_t ctx = {
        .array = peer_array,
        .max_peers = max_peers,
        .count = 0
    };

    hashmap_foreach(sprout_net.peers_map, _list_peers_callback, &ctx);
    return ctx.count;
}

typedef struct {
    uint32_t stale_timeout_ms;
    size_t removed_count;
} route_cleanup_ctx_t;

static void _cleanup_stale_routes_callback(void *key, void *value, void *user_data)
{
    uint8_t *mac = (uint8_t *)key;
    SPROUT_STORE_HASH_TYPE *peer = (SPROUT_STORE_HASH_TYPE *)value;
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

size_t sprout_cleanup_stale_routes(uint32_t timeout_ms)
{
    route_cleanup_ctx_t ctx = {
        .stale_timeout_ms = timeout_ms,
        .removed_count = 0
    };

    hashmap_foreach(sprout_net.peers_map, _cleanup_stale_routes_callback, &ctx);

    if (ctx.removed_count > 0) {
        ESP_LOGI(TAG, "Cleaned up %zu stale routes", ctx.removed_count);
    }

    return ctx.removed_count;
}

void sprout_print_route_table(void)
{
    size_t peer_count = hashmap_size(sprout_net.peers_map);
    ESP_LOGI(TAG, "========== ROUTE TABLE (%zu peers) ==========", peer_count);

    if (peer_count == 0) {
        ESP_LOGI(TAG, "  (empty)");
        return;
    }

    sprout_peer_info_t *peers = heap_caps_malloc(peer_count * sizeof(sprout_peer_info_t), MALLOC_CAP_DEFAULT);
    if (!peers) {
        ESP_LOGE(TAG, "Failed to allocate memory for route table");
        return;
    }

    size_t actual = sprout_list_all_peers(peers, peer_count);
    for (size_t i = 0; i < actual; i++) {
        const char *state_str = "UNKNOWN";
        switch (peers[i].state) {
            case SPROUT_PEER_STATE_DISCOVERED: state_str = "discovered"; break;
            case SPROUT_PEER_STATE_PENDING: state_str = "PENDING"; break;
            case SPROUT_PEER_STATE_CONNECTED: state_str = "CONNECTED"; break;
            case SPROUT_PEER_STATE_REJECTED: state_str = "rejected"; break;
            case SPROUT_PEER_STATE_BLOCKED: state_str = "BLOCKED"; break;
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

typedef struct {
    const char *name;
    uint8_t *mac_out;
    bool found;
} peer_name_search_ctx_t;

static void _find_peer_by_name_callback(void *key, void *value, void *user_data)
{
    (void)key;
    SPROUT_STORE_HASH_TYPE *peer = (SPROUT_STORE_HASH_TYPE *)value;
    peer_name_search_ctx_t *ctx = (peer_name_search_ctx_t *)user_data;

    if (peer && !ctx->found) {
        if (strncmp(peer->name, ctx->name, PEER_NAME_MAX_LENGTH) == 0) {
            memcpy(ctx->mac_out, peer->peer_info.peer_addr, MAC_ADDRESS_LENGTH);
            ctx->found = true;
        }
    }
}

esp_err_t sprout_get_peer_by_name(const char *peer_name, uint8_t *mac_out)
{
    ESP_RETURN_ON_FALSE(peer_name != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer name is NULL");
    ESP_RETURN_ON_FALSE(mac_out != NULL, ESP_ERR_INVALID_ARG, TAG, "MAC output buffer is NULL");

    peer_name_search_ctx_t ctx = {
        .name = peer_name,
        .mac_out = mac_out,
        .found = false
    };

    hashmap_foreach(sprout_net.peers_map, _find_peer_by_name_callback, &ctx);

    if (!ctx.found) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static void _clear_all_peers_callback(void *key, void *value, void *user_data)
{
    (void)user_data;
    uint8_t *mac = (uint8_t *)key;
    SPROUT_STORE_HASH_TYPE *peer = (SPROUT_STORE_HASH_TYPE *)value;

    if (peer) {
        esp_now_del_peer(mac);

        if (peer->response_queue != NULL) {
            vQueueDelete(peer->response_queue);
        }
    }
}

esp_err_t sprout_clear_all_peers(void)
{
    size_t peer_count = hashmap_size(sprout_net.peers_map);

    if (peer_count == 0) {
        ESP_LOGI(TAG, "No peers to clear");
        return ESP_OK;
    }

    hashmap_foreach(sprout_net.peers_map, _clear_all_peers_callback, NULL);

    hashmap_clear(sprout_net.peers_map);

    ESP_LOGI(TAG, "Cleared %zu peers", peer_count);
    return ESP_OK;
}

bool sprout_is_peer_reachable(uint8_t *peer_mac, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, false, TAG, "Peer MAC is NULL");

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    if (peer == NULL) {
        ESP_LOGW(TAG, "Peer not found in peer map");
        return false;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t age_us = now_us - peer->last_seen;
    uint64_t age_ms = (uint64_t)US_TO_MS(age_us);

    if (age_ms <= timeout_ms) {
        return true;
    }

    esp_err_t err = sprout_send_device_info(peer_mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send ping to peer: %s", esp_err_to_name(err));
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    int64_t initial_last_seen = peer->last_seen;

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        peer = _get_peer_from_map(peer_mac);
        if (peer && peer->last_seen > initial_last_seen) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return false;
}

size_t sprout_get_peers(sprout_data_receive_cb_t callback, void *user_data)
{
    if (callback) {
        hashmap_foreach(sprout_net.peers_map, callback, user_data);
        return hashmap_size(sprout_net.peers_map);
    }
    else {
        return 0;
    }
}
