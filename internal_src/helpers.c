/**
 * @file helpers.c
 * @brief FPR Internal Helper Functions
 * 
 * Internal utility functions for peer management, data storage,
 * and packet handling. These functions are not part of the public API.
 * 
 * @version 1.0.0
 * @date December 2024
 */

#include "fpr/internal/helpers.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "fpr_helpers";

/* Returns true when the packet should be enqueued/callbacked, false when
 * the queue-mode policy dropped it. Callers must honor the return: the
 * drops counted here previously still reached the queue. */
static bool _store_data_with_mode(fpr_node_t *net, FPR_STORE_HASH_TYPE *store, const fpr_package_t *data, uint8_t *peer_address) 
{
    // Determine packet type characteristics
    bool is_single_packet = (data->package_type == FPR_PACKAGE_TYPE_SINGLE);
    bool is_fragment_start = (data->package_type == FPR_PACKAGE_TYPE_START);
    bool is_fragment_middle = (data->package_type == FPR_PACKAGE_TYPE_CONTINUED);
    bool is_fragment_end = (data->package_type == FPR_PACKAGE_TYPE_END);
    bool is_fragmented = (is_fragment_start || is_fragment_middle || is_fragment_end);
    
    // CRITICAL: Always accept control/handshake packets regardless of queue mode
    // Control packets (id == -1) are essential for connection management
    bool is_control_packet = (data->id == FPR_PACKET_ID_CONTROL);
    
    // Handle LATEST_ONLY mode - only accept single packets, reject fragmented data
    // BUT always allow control packets through
    if (store->queue_mode == FPR_QUEUE_MODE_LATEST_ONLY && !is_control_packet) {
        if (is_fragmented) {
            // In latest-only mode, fragmented packets are not supported
            // because we can't guarantee all fragments arrive before newer data
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Latest-only mode: dropping fragmented packet from " MACSTR 
                     " (type=%d). Use NORMAL mode for large data.",
                     MAC2STR(peer_address), data->package_type);
            #endif
            net->stats.packets_dropped++;
            // Reset any partial fragment state
            store->receiving_fragmented = false;
            store->fragment_seq_num = 0;
            return false;
        }

        // Single packet in latest-only mode - drain queue before adding
        if (is_single_packet && store->queued_packets > 0 && store->response_queue != NULL) {
            fpr_package_t discard_pkg;
            uint32_t dropped = 0;
            // Drain all existing packets from queue and count them
            while (xQueueReceive(store->response_queue, &discard_pkg, 0) == pdPASS) {
                dropped++;
            }
            store->queued_packets = 0;
            if (dropped > 0) {
                net->stats.packets_dropped += dropped;
            }
        }
    } else if (!is_control_packet) {
        // NORMAL mode - handle fragmented packets properly
        if (is_fragment_start) {
            // Starting a new fragmented message
            // If we were already receiving fragments, the old message is lost
            if (store->receiving_fragmented) {
                #if (FPR_DEBUG == 1)
                ESP_LOGW(TAG, "New fragment sequence started, discarding incomplete previous message");
                #endif
                // Drain partial fragments from queue
                fpr_package_t discard_pkg;
                while (xQueueReceive(store->response_queue, &discard_pkg, 0) == pdPASS) {
                    // Check if this was part of old incomplete sequence
                    if (discard_pkg.sequence_num == store->fragment_seq_num) {
                        net->stats.packets_dropped++;
                    } else {
                        // This was a complete packet, put it back (shouldn't happen often)
                        xQueueSendToFront(store->response_queue, &discard_pkg, 0);
                        break;
                    }
                }
            }
            store->receiving_fragmented = true;
            store->fragment_seq_num = data->sequence_num;
        } else if (is_fragment_middle || is_fragment_end) {
            // Check if this fragment belongs to current sequence
            if (!store->receiving_fragmented || data->sequence_num != store->fragment_seq_num) {
                // Orphaned fragment - drop it
                #if (FPR_DEBUG == 1)
                ESP_LOGW(TAG, "Dropping orphaned fragment from " MACSTR " (expected seq %lu, got %lu)",
                         MAC2STR(peer_address), (unsigned long)store->fragment_seq_num,
                         (unsigned long)data->sequence_num);
                #endif
                net->stats.packets_dropped++;
                return false;
            }
            if (is_fragment_end) {
                // Fragment sequence complete
                store->receiving_fragmented = false;
                store->fragment_seq_num = 0;
            }
        }
    }
    return true;
}

void _peer_store_data(fpr_node_t *net, const fpr_rx_frame_t *frame, const fpr_package_t *data)
{
    net->stats.packets_received++;
    const uint8_t *peer_address = frame->src_mac;
    /* Recursive: callers in the RX path already hold peers_lock, and the
     * on_data callback below may call fpr_remove_peer() which takes
     * it again on this same task. */
    _peers_lock(net);
    FPR_STORE_HASH_TYPE *store = _peer_from_map(net, peer_address);
    
    if (store && store->state == FPR_PEER_STATE_CONNECTED) {
        _peer_touch(store, frame);
        
        // Replay protection: wrap-aware sequence check.
        // - seq 0 is always allowed (legacy/handshake packets)
        // - a "newer" sequence (within half the space, so a forged
        //   UINT32_MAX cannot pin the watermark forever) is accepted
        // - an equal sequence is only a retransmitted fragment of the
        //   in-progress multi-packet message; equal-seq singles are replays
        if (data->sequence_num != 0) {
            int32_t seq_diff = (int32_t)(data->sequence_num - store->last_seq_num);
            bool is_newer = (seq_diff > 0);
            bool is_fragment_resend = (seq_diff == 0) && store->receiving_fragmented;
            if (!is_newer && !is_fragment_resend) {
                // Potential replay attack - drop packet with old sequence
                net->stats.replay_attacks_blocked++;
                #if (FPR_DEBUG == 1)
                ESP_LOGW(TAG, "Replay attack blocked from " MACSTR " (seq %lu, last %lu)",
                         MAC2STR(peer_address), (unsigned long)data->sequence_num,
                         (unsigned long)store->last_seq_num);
                #endif
                _peers_unlock(net);
                return;
            }

            if (is_newer) {
                store->last_seq_num = data->sequence_num;
            }
        }

        store->packets_received++;

        // Control/handshake packets (id == -1) are consumed by the receive
        // handlers themselves; they carry key material and must never reach
        // the application queue or the data callback.
        bool is_control_packet = (data->id == FPR_PACKET_ID_CONTROL);
        if (is_control_packet) {
            _peers_unlock(net);
            return;
        }

        // Determine if this is a complete packet
        bool is_complete_packet = (data->package_type == FPR_PACKAGE_TYPE_SINGLE ||
                                   data->package_type == FPR_PACKAGE_TYPE_END);

        // Queue-mode policy may drop the packet (counted already).
        if (!_store_data_with_mode(net, store, data, peer_address)) {
            _peers_unlock(net);
            return;
        }
        
        // Store in queue (non-blocking) - do not block the receiver on queue availability
        if (xQueueSend(store->response_queue, (void*)data, 0) == pdPASS) {
            // Increment queued packet count only for complete packets
            if (is_complete_packet) {
                store->queued_packets++;
            }
        } else {
            // Queue full - increment dropped counter
            net->stats.packets_dropped++;
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Queue full, packet dropped from " MACSTR, MAC2STR(peer_address));
            #endif
        }

        /* Finish every store/queue access before invoking application
         * code: a callback may remove this peer or clear the peer map. */
        if (net->callbacks.on_data) {
            fpr_package_t *package = (fpr_package_t *)data;
            const size_t cap = sizeof(package->protocol);
            int data_len = (int)((data->payload_size > 0 && data->payload_size <= cap)
                                 ? data->payload_size : cap);
            _cb_data(net, peer_address, &package->protocol, data_len);
        }
    }
    _peers_unlock(net);
}

esp_err_t _peer_add(fpr_node_t *net, const uint8_t *peer_mac, const char *name, bool is_connected)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    _peers_lock(net);
    /* Quiesce gate: fpr_deinit flips state to UNINITIALIZED
     * before emptying the maps — a callback still in flight from before
     * esp_now_deinit must not repopulate what teardown just cleared.
     * Every other state (INITIALIZED/STARTED/PAUSED/STOPPED) keeps the
     * maps live and may add peers. */
    if (net->state == FPR_STATE_UNINITIALIZED) {
        _peers_unlock(net);
        return ESP_ERR_INVALID_STATE;
    }
    /* hashmap_put replaces values but keeps the original key pointer.
     * Replacing an owned store would leak it and its response queue. */
    if (_peer_from_map(net, peer_mac) != NULL) {
        _peers_unlock(net);
        ESP_LOGE(TAG, "Peer already exists");
        return ESP_ERR_INVALID_STATE;
    }

    FPR_STORE_HASH_TYPE *store = (FPR_STORE_HASH_TYPE *)heap_caps_calloc(1, sizeof(FPR_STORE_HASH_TYPE), MALLOC_CAP_DEFAULT);
    if (!store) {
        _peers_unlock(net);
        ESP_LOGE(TAG, "Failed to allocate peer store");
        return ESP_ERR_NO_MEM;
    }
    
    _safe_string_copy(store->name, name ? name : "Unnamed", sizeof(store->name));
    store->response_queue = xQueueCreate(FPR_QUEUE_LENGTH, sizeof(fpr_package_t));
    if (!store->response_queue) {
        _peer_store_free_or_defer(store);
        _peers_unlock(net);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize security
    fpr_security_init_keys(&store->security);
    store->sec_state = FPR_SEC_STATE_NONE;
    
    store->is_connected = is_connected;
    store->state = is_connected ? FPR_PEER_STATE_CONNECTED : FPR_PEER_STATE_DISCOVERED;
    store->hop_count = 0;
    memset(store->next_hop_mac, 0, 6);
    store->last_seen = esp_timer_get_time();
    store->rssi = 0;
    store->packets_received = 0;
    store->queued_packets = 0;
    store->queue_mode = net->default_queue_mode;
    store->receiving_fragmented = false;
    store->fragment_seq_num = 0;
    
    // Keep a persistent MAC copy — it is the hashmap key, so it must
    // outlive the caller's buffer.
    memcpy(store->mac, peer_mac, MAC_ADDRESS_LENGTH);

    bool success = hashmap_put(&net->peers_map, store->mac, store);
    if (success) {
        const fpr_transport_t *t = _transport(net);
        esp_err_t err = t ? t->add_peer(t->ctx, store->mac, 0, false)
                          : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            hashmap_remove(&net->peers_map, store->mac);
            _peer_store_free_or_defer(store);
            _peers_unlock(net);
            return err;
        }
        _peers_unlock(net);
        return ESP_OK;
    }
    else {
        _peer_store_free_or_defer(store);
        _peers_unlock(net);
        return ESP_FAIL;
    }
}

// used by multiple modes
esp_err_t _peer_add_discovered(fpr_node_t *net, const char *name, const uint8_t *address, bool is_connected)
{
    ESP_RETURN_ON_FALSE(name != NULL && address != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    return _peer_add(net, address, name, is_connected);
}

// Helper: Copy peer data to info structure
void _peer_copy_to_info(const FPR_STORE_HASH_TYPE *peer, fpr_peer_info_t *info)
{
    memcpy(info->mac, peer->mac, MAC_ADDRESS_LENGTH);
    _safe_string_copy(info->name, peer->name, sizeof(info->name));
    info->is_connected = peer->is_connected;
    info->state = peer->state;
    info->hop_count = peer->hop_count;
    info->rssi = peer->rssi;
    info->last_seen_ms = (uint64_t)FPR_US_TO_MS(esp_timer_get_time() - peer->last_seen);
    info->packets_received = peer->packets_received;
}
// ========== PEER STORE / MAP OPERATIONS ==========

esp_err_t _peer_remove(fpr_node_t *net, const uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    _peers_lock(net);
    /* The map key may point inside the store we are about to release. */
    uint8_t mac[MAC_ADDRESS_LENGTH];
    memcpy(mac, peer_mac, sizeof(mac));
    FPR_STORE_HASH_TYPE *var = _peer_from_map(net, mac);
    esp_err_t result;
    if (var != NULL) {
        if (!hashmap_remove(&net->peers_map, mac)) {
            _peers_unlock(net);
            return ESP_FAIL;
        }
        _peer_store_free_or_defer(var);
    }
    const fpr_transport_t *t = _transport(net);
    result = t ? t->del_peer(t->ctx, mac) : ESP_ERR_INVALID_STATE;
    _peers_unlock(net);
    return result;
}

void _route_free_cb(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    heap_caps_free(value);
}

void _peer_collect_cb(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    fpr_peer_collect_ctx_t *ctx = (fpr_peer_collect_ctx_t *)user_data;

    if (peer && ctx->count < ctx->max_peers) {
        if (ctx->connected_only && peer->state != FPR_PEER_STATE_CONNECTED) {
            return;
        }
        _peer_copy_to_info(peer, &ctx->array[ctx->count]);
        ctx->count++;
    }
}