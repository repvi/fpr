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
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "fpr_diag";
code_version_t fpr_get_protocol_version(void)
{
    return FPR_PROTOCOL_VERSION;
}

void fpr_get_protocol_version_string(char *buf, size_t buf_size)
{
    if (buf && buf_size > 0) {
        snprintf(buf, buf_size, "%" PRId32 ".%" PRId32 ".%" PRId32,
                 (uint32_t)CODE_VERSION_MAJOR(FPR_PROTOCOL_VERSION),
                 (uint32_t)CODE_VERSION_MINOR(FPR_PROTOCOL_VERSION),
                 (uint32_t)CODE_VERSION_PATCH(FPR_PROTOCOL_VERSION));
    }
}

void fpr_get_network_stats(fpr_network_stats_t *stats)
{
    if (stats) {
        /* Counters are written under peers_lock by the rx/send paths —
         * read the whole block inside it or the snapshot can tear. */
        _peers_lock();
        stats->packets_sent = fpr_net.stats.packets_sent;
        stats->packets_received = fpr_net.stats.packets_received;
        stats->packets_forwarded = fpr_net.stats.packets_forwarded;
        stats->packets_dropped = fpr_net.stats.packets_dropped;
        stats->send_failures = fpr_net.stats.send_failures;
        stats->replay_attacks_blocked = fpr_net.stats.replay_attacks_blocked;
        stats->peer_count = hashmap_size(&fpr_net.peers_map);
        _peers_unlock();
    }
}

void fpr_reset_network_stats(void)
{
    _peers_lock();
    memset(&fpr_net.stats, 0, sizeof(fpr_net.stats));
    _peers_unlock();
    ESP_LOGI(TAG, "Network statistics reset");
}

typedef struct {
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
            _peer_remove(mac);
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
    hashmap_foreach(&fpr_net.peers_map, _route_peer_stale_cb, &ctx);
    hashmap_foreach(&fpr_net.routes_map, _route_stale_cb, &ctx);
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
