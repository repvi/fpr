/**
 * @file helpers.c
 * @brief Sprout Internal Helper Functions
 *
 * Internal utility functions for peer management, data storage,
 * and packet handling. These functions are not part of the public API.
 */

#include "sprout/internal/helpers.h"
#include "sprout/internal/private_defs.h"
#include "sprout/sprout_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "string.h"

static const char *TAG = "sprout_helpers";

extern sprout_network_t sprout_net;

esp_err_t _remove_peer_internal(uint8_t *peer_mac)
{
    SPROUT_STORE_HASH_TYPE *var = _get_peer_from_map(peer_mac);
    if (var != NULL) {
        if (!hashmap_remove(sprout_net.peers_map, peer_mac)) {
            return ESP_FAIL;
        }
        vQueueDelete(var->response_queue);
        heap_caps_free(var);
    }
    return esp_now_del_peer(peer_mac);
}

void _cleanup_peer_entry(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    SPROUT_STORE_HASH_TYPE *peer = (SPROUT_STORE_HASH_TYPE *)value;
    if (peer) {
        if (peer->response_queue) {
            vQueueDelete(peer->response_queue);
        }
        esp_now_del_peer(peer->peer_info.peer_addr);
        heap_caps_free(peer);
    }
}

void _reset_all_peers(void)
{
    hashmap_foreach(sprout_net.peers_map, _cleanup_peer_entry, NULL);
}

static void _store_data_with_mode(SPROUT_STORE_HASH_TYPE *store, const sprout_package_t *data, uint8_t *peer_address)
{
    bool is_single_packet = (data->package_type == SPROUT_PACKAGE_TYPE_SINGLE);
    bool is_fragment_start = (data->package_type == SPROUT_PACKAGE_TYPE_START);
    bool is_fragment_middle = (data->package_type == SPROUT_PACKAGE_TYPE_CONTINUED);
    bool is_fragment_end = (data->package_type == SPROUT_PACKAGE_TYPE_END);
    bool is_fragmented = (is_fragment_start || is_fragment_middle || is_fragment_end);

    bool is_control_packet = (data->id == SPROUT_PACKET_ID_CONTROL);

    if (store->queue_mode == SPROUT_QUEUE_MODE_LATEST_ONLY && !is_control_packet) {
        if (is_fragmented) {
            #if (SPROUT_DEBUG == 1)
            ESP_LOGW(TAG, "Latest-only mode: dropping fragmented packet from " MACSTR
                     " (type=%d). Use NORMAL mode for large data.",
                     MAC2STR(peer_address), data->package_type);
            #endif
            sprout_net.stats.packets_dropped++;
            store->receiving_fragmented = false;
            store->fragment_seq_num = 0;
            return;
        }

        if (is_single_packet && store->queued_packets > 0 && store->response_queue != NULL) {
            sprout_package_t discard_pkg;
            uint32_t dropped = 0;
            while (xQueueReceive(store->response_queue, &discard_pkg, 0) == pdPASS) {
                dropped++;
            }
            store->queued_packets = 0;
            if (dropped > 0) {
                sprout_net.stats.packets_dropped += dropped;
            }
        }
    } else if (!is_control_packet) {
        if (is_fragment_start) {
            if (store->receiving_fragmented) {
                #if (SPROUT_DEBUG == 1)
                ESP_LOGW(TAG, "New fragment sequence started, discarding incomplete previous message");
                #endif
                sprout_package_t discard_pkg;
                while (xQueueReceive(store->response_queue, &discard_pkg, 0) == pdPASS) {
                    if (discard_pkg.sequence_num == store->fragment_seq_num) {
                        sprout_net.stats.packets_dropped++;
                    } else {
                        xQueueSendToFront(store->response_queue, &discard_pkg, 0);
                        break;
                    }
                }
            }
            store->receiving_fragmented = true;
            store->fragment_seq_num = data->sequence_num;
        } else if (is_fragment_middle || is_fragment_end) {
            if (!store->receiving_fragmented || data->sequence_num != store->fragment_seq_num) {
                #if (SPROUT_DEBUG == 1)
                ESP_LOGW(TAG, "Dropping orphaned fragment from " MACSTR " (expected seq %lu, got %lu)",
                         MAC2STR(peer_address), (unsigned long)store->fragment_seq_num,
                         (unsigned long)data->sequence_num);
                #endif
                sprout_net.stats.packets_dropped++;
                return;
            }
            if (is_fragment_end) {
                store->receiving_fragmented = false;
                store->fragment_seq_num = 0;
            }
        }
    }
}

void _store_data_from_peer_helper(const esp_now_recv_info_t *esp_now_info, const sprout_package_t *data)
{
    sprout_net.stats.packets_received++;
    uint8_t *peer_address = (uint8_t *)esp_now_info->src_addr;
    SPROUT_STORE_HASH_TYPE *store = _get_peer_from_map(peer_address);

    if (store && store->state == SPROUT_PEER_STATE_CONNECTED) {
        _update_peer_rssi_and_timestamp(store, esp_now_info);

        if (data->sequence_num != 0 && data->sequence_num < store->last_seq_num) {
            sprout_net.stats.replay_attacks_blocked++;
            #if (SPROUT_DEBUG == 1)
            ESP_LOGW(TAG, "Replay attack blocked from " MACSTR " (seq %lu < last %lu)",
                     MAC2STR(peer_address), (unsigned long)data->sequence_num,
                     (unsigned long)store->last_seq_num);
            #endif
            return;
        }

        if (data->sequence_num > store->last_seq_num) {
            store->last_seq_num = data->sequence_num;
        }

        store->packets_received++;

        bool is_control_packet = (data->id == SPROUT_PACKET_ID_CONTROL);

        bool is_complete_packet = (data->package_type == SPROUT_PACKAGE_TYPE_SINGLE ||
                                   data->package_type == SPROUT_PACKAGE_TYPE_END);

        if (!is_control_packet) {
            _store_data_with_mode(store, data, peer_address);
        }

        if (sprout_net.data_callback) {
            sprout_package_t *package = (sprout_package_t *)data;
            int data_len = (int)sizeof(package->protocol);
            sprout_net.data_callback(peer_address, &package->protocol, &data_len);
        }

        if (xQueueSend(store->response_queue, (void*)data, 0) == pdPASS) {
            if (is_complete_packet) {
                store->queued_packets++;
            }
        } else {
            sprout_net.stats.packets_dropped++;
            #if (SPROUT_DEBUG == 1)
            ESP_LOGW(TAG, "Queue full, packet dropped from " MACSTR, MAC2STR(peer_address));
            #endif
        }
    }
}

esp_err_t _add_peer_internal(uint8_t *peer_mac, const char *name, bool is_connected, uint32_t key)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    SPROUT_STORE_HASH_TYPE *store = (SPROUT_STORE_HASH_TYPE *)heap_caps_calloc(1, sizeof(SPROUT_STORE_HASH_TYPE), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(store != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate peer store");

    _safe_string_copy(store->name, name ? name : "Unnamed", sizeof(store->name));
    store->response_queue = xQueueCreate(SPROUT_QUEUE_LENGTH, sizeof(sprout_package_t));
    if (!store->response_queue) {
        heap_caps_free(store);
        return ESP_ERR_NO_MEM;
    }

    sprout_security_init_keys(&store->security);
    store->sec_state = SPROUT_SEC_STATE_NONE;

    store->is_connected = is_connected;
    store->state = is_connected ? SPROUT_PEER_STATE_CONNECTED : SPROUT_PEER_STATE_DISCOVERED;
    store->hop_count = 0;
    memset(store->next_hop_mac, 0, 6);
    store->last_seen = esp_timer_get_time();
    store->rssi = 0;
    store->packets_received = 0;
    store->queued_packets = 0;
    store->queue_mode = sprout_net.default_queue_mode;
    store->receiving_fragmented = false;
    store->fragment_seq_num = 0;

    memcpy(store->peer_info.peer_addr, peer_mac, 6);
    sprout_set_peer_info(&store->peer_info);

    bool success = hashmap_put(sprout_net.peers_map, store->peer_info.peer_addr, store);
    if (success) {
        esp_now_del_peer(store->peer_info.peer_addr);
        esp_err_t err = esp_now_add_peer(&store->peer_info);
        if (err != ESP_OK) {
            hashmap_remove(sprout_net.peers_map, store->peer_info.peer_addr);
            vQueueDelete(store->response_queue);
            heap_caps_free(store);
            return err;
        }
        return ESP_OK;
    }
    else {
        vQueueDelete(store->response_queue);
        heap_caps_free(store);
        return ESP_FAIL;
    }
}

esp_err_t _add_discovered_peer(const char *name, uint8_t *address, uint32_t key, bool is_connected)
{
    ESP_RETURN_ON_FALSE(name != NULL && address != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    return _add_peer_internal(address, name, is_connected, key);
}

void _copy_peer_to_info(const SPROUT_STORE_HASH_TYPE *peer, sprout_peer_info_t *info)
{
    memcpy(info->mac, peer->peer_info.peer_addr, 6);
    _safe_string_copy(info->name, peer->name, sizeof(info->name));
    info->is_connected = peer->is_connected;
    info->state = peer->state;
    info->hop_count = peer->hop_count;
    info->rssi = peer->rssi;
    info->last_seen_ms = (uint64_t)US_TO_MS(esp_timer_get_time() - peer->last_seen);
    info->packets_received = peer->packets_received;
}