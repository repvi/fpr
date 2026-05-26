/**
 * @file state.c
 * @brief Sprout Network State, Power, Queue, and Statistics
 *
 * Network pause/resume, state queries, power mode, channel,
 * queue mode, and statistics tracking.
 */

#include "sprout/internal/helpers.h"
#include "sprout/sprout.h"
#include "sprout/internal/private_defs.h"
#include "sprout/sprout_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "sprout_state";

extern sprout_network_t sprout_net;


esp_err_t sprout_set_peer_queue_mode(uint8_t *peer_mac, sprout_queue_mode_t mode)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    if (peer) {
        peer->queue_mode = mode;
        ESP_LOGI(TAG, "Queue mode for peer " MACSTR " set to %s",
                 MAC2STR(peer_mac), mode == SPROUT_QUEUE_MODE_LATEST_ONLY ? "LATEST_ONLY" : "NORMAL");
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

uint32_t sprout_get_peer_queued_packets(uint8_t *peer_mac)
{
    if (peer_mac == NULL) {
        return 0;
    }

    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    if (peer) {
        return peer->queued_packets;
    }
    return 0;
}

void sprout_get_network_stats(sprout_network_stats_t *stats)
{
    if (stats) {
        stats->packets_sent = sprout_net.stats.packets_sent;
        stats->packets_received = sprout_net.stats.packets_received;
        stats->packets_dropped = sprout_net.stats.packets_dropped;
        stats->send_failures = sprout_net.stats.send_failures;
        stats->peer_count = hashmap_size(sprout_net.peers_map);
    }
}

void sprout_reset_network_stats(void)
{
    memset(&sprout_net.stats, 0, sizeof(sprout_net.stats));
    ESP_LOGI(TAG, "Network statistics reset");
}
