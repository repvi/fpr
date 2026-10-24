/**
 * @file fpr_diag.c
 * @brief FPR Diagnostics: protocol version, network statistics, route table
 */
#include "fpr/fpr.h"
#include "fpr/internal/helpers.h"
#include "fpr/fpr_lts.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "fpr_diag";
#define FPR_REQUIRE_NODE(n) \
    ESP_RETURN_ON_FALSE((n) != NULL, ESP_ERR_INVALID_ARG, TAG, "Node is NULL")
code_version_t fpr_get_protocol_version(void)
{
    return FPR_PROTOCOL_VERSION;
}

esp_err_t fpr_get_protocol_version_string(char *buf, size_t buf_size)
{
    ESP_RETURN_ON_FALSE(buf != NULL && buf_size > 0, ESP_ERR_INVALID_ARG,
                        TAG, "Invalid buffer");
    snprintf(buf, buf_size, "%" PRId32 ".%" PRId32 ".%" PRId32,
             (uint32_t)CODE_VERSION_MAJOR(FPR_PROTOCOL_VERSION),
             (uint32_t)CODE_VERSION_MINOR(FPR_PROTOCOL_VERSION),
             (uint32_t)CODE_VERSION_PATCH(FPR_PROTOCOL_VERSION));
    return ESP_OK;
}

esp_err_t fpr_get_stats(fpr_node_t *node, fpr_stats_t *stats)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(stats != NULL, ESP_ERR_INVALID_ARG, TAG, "Stats is NULL");
    {
        /* Counters are written under peers_lock by the rx/send paths —
         * read the whole block inside it or the snapshot can tear. */
        _peers_lock(node);
        stats->packets_sent = node->stats.packets_sent;
        stats->packets_received = node->stats.packets_received;
        stats->packets_forwarded = node->stats.packets_forwarded;
        stats->packets_dropped = node->stats.packets_dropped;
        stats->send_failures = node->stats.send_failures;
        stats->replay_attacks_blocked = node->stats.replay_attacks_blocked;
        stats->peer_count = hashmap_size(&node->peers_map);
        _peers_unlock(node);
    }
    return ESP_OK;
}

esp_err_t fpr_reset_stats(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    _peers_lock(node);
    memset(&node->stats, 0, sizeof(node->stats));
    _peers_unlock(node);
    ESP_LOGI(TAG, "Network statistics reset");
    return ESP_OK;
}

typedef struct {
    fpr_node_t *node;
    uint32_t stale_timeout_ms;
    size_t removed_count;
} route_cleanup_ctx_t;

static void _route_peer_stale_cb(void *key, void *value, void *user_data)
{
    const uint8_t *mac = (const uint8_t *)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    route_cleanup_ctx_t *ctx = (route_cleanup_ctx_t *)user_data;

    if (peer) {
        int64_t age = esp_timer_get_time() - peer->last_seen;
        uint64_t age_ms = (uint64_t)FPR_US_TO_MS(age);
        if (age_ms > ctx->stale_timeout_ms) {
            ESP_LOGI(TAG, "Removing stale route to " MACSTR " (age: %llu ms)",
                     MAC2STR(mac), (unsigned long long)age_ms);
            _peer_remove(ctx->node, mac);
            ctx->removed_count++;
        }
    }
}

static void _route_stale_cb(void *key, void *value, void *user_data)
{
    const uint8_t *mac = (const uint8_t *)key;
    fpr_route_entry_t *route = (fpr_route_entry_t *)value;
    route_cleanup_ctx_t *ctx = (route_cleanup_ctx_t *)user_data;

    if (route) {
        int64_t age = esp_timer_get_time() - route->last_seen;
        uint64_t age_ms = (uint64_t)FPR_US_TO_MS(age);
        if (age_ms > ctx->stale_timeout_ms) {
            ESP_LOGI(TAG, "Removing stale extender route to " MACSTR " (age: %llu ms)",
                     MAC2STR(mac), (unsigned long long)age_ms);
            hashmap_remove(&ctx->node->routes_map, mac);
            heap_caps_free(route);
            ctx->removed_count++;
        }
    }
}

esp_err_t fpr_prune_stale(fpr_node_t *node, uint32_t timeout_ms, size_t *removed_out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(removed_out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    route_cleanup_ctx_t ctx = {
        .node = node,
        .stale_timeout_ms = timeout_ms,
        .removed_count = 0
    };

    _peers_lock(node);
    hashmap_foreach(&node->peers_map, _route_peer_stale_cb, &ctx);
    hashmap_foreach(&node->routes_map, _route_stale_cb, &ctx);
    _peers_unlock(node);

    if (ctx.removed_count > 0) {
        ESP_LOGI(TAG, "Cleaned up %zu stale routes", ctx.removed_count);
    }
    *removed_out = ctx.removed_count;
    return ESP_OK;
}

esp_err_t fpr_print_peers(fpr_node_t *node)
{
    FPR_REQUIRE_NODE(node);
    _peers_lock(node);
    size_t peer_count = hashmap_size(&node->peers_map);
    ESP_LOGI(TAG, "========== ROUTE TABLE (%zu peers) ==========", peer_count);

    if (peer_count == 0) {
        _peers_unlock(node);
        ESP_LOGI(TAG, "  (empty)");
        return ESP_OK;
    }
    _peers_unlock(node);

    fpr_peer_info_t *peers = heap_caps_malloc(peer_count * sizeof(fpr_peer_info_t), MALLOC_CAP_DEFAULT);
    if (!peers) {
        ESP_LOGE(TAG, "Failed to allocate memory for route table");
        return ESP_ERR_NO_MEM;
    }

    size_t actual = 0;
    (void)fpr_list_peers(node, peers, peer_count, &actual);
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
    return ESP_OK;
}
