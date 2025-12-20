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
#include "esp_log.h"

static const char *TAG = "fpr_helpers";

static void _store_data_with_mode(FPR_STORE_HASH_TYPE *store, const fpr_package_t *data, uint8_t *peer_address) 
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
            fpr_net.stats.packets_dropped++;
            // Reset any partial fragment state
            store->receiving_fragmented = false;
            store->fragment_seq_num = 0;
            return;
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
                fpr_net.stats.packets_dropped += dropped;
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
                        fpr_net.stats.packets_dropped++;
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
                fpr_net.stats.packets_dropped++;
                return;
            }
            if (is_fragment_end) {
                // Fragment sequence complete
                store->receiving_fragmented = false;
                store->fragment_seq_num = 0;
            }
        }
    }
}

void _store_data_from_peer_helper(const esp_now_recv_info_t *esp_now_info, const fpr_package_t *data) 
{
    fpr_net.stats.packets_received++;
    uint8_t *peer_address = (uint8_t *)esp_now_info->src_addr;
    FPR_STORE_HASH_TYPE *store = _get_peer_from_map(peer_address);
    
    if (store && store->state == FPR_PEER_STATE_CONNECTED) {
        _update_peer_rssi_and_timestamp(store, esp_now_info);
        
        // Replay protection: check sequence number
        // Allow sequence 0 (for legacy/handshake packets)
        // Allow same sequence (for multi-packet fragments)
        // Block packets with OLDER sequence numbers (replay attacks)
        if (data->sequence_num != 0 && data->sequence_num < store->last_seq_num) {
            // Potential replay attack - drop packet with old sequence
            fpr_net.stats.replay_attacks_blocked++;
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Replay attack blocked from " MACSTR " (seq %lu < last %lu)",
                     MAC2STR(peer_address), (unsigned long)data->sequence_num, 
                     (unsigned long)store->last_seq_num);
            #endif
            return;
        }
        
        // Update last seen sequence number (only if newer)
        if (data->sequence_num > store->last_seq_num) {
            store->last_seq_num = data->sequence_num;
        }
        
        store->packets_received++;

        // Check if this is a control packet
        bool is_control_packet = (data->id == FPR_PACKET_ID_CONTROL);
        
        // Determine if this is a complete packet
        bool is_complete_packet = (data->package_type == FPR_PACKAGE_TYPE_SINGLE || 
                                   data->package_type == FPR_PACKAGE_TYPE_END);
        
        // Handle queue mode logic (but skip for control packets - they always go through)
        if (!is_control_packet) {
            _store_data_with_mode(store, data, peer_address);
        }
        
        // Call application callback if registered (do this before queue send to avoid blocking delays)
        if (fpr_net.data_callback) {
            fpr_package_t *package = (fpr_package_t *)data;
            // Pass the protocol payload size to the application callback
            int data_len = (int)sizeof(package->protocol);
            fpr_net.data_callback(peer_address, &package->protocol, &data_len);
        }

        // Store in queue (non-blocking) - do not block the receiver on queue availability
        if (xQueueSend(store->response_queue, (void*)data, 0) == pdPASS) {
            // Increment queued packet count only for complete packets
            if (is_complete_packet) {
                store->queued_packets++;
            }
        } else {
            // Queue full - increment dropped counter
            fpr_net.stats.packets_dropped++;
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Queue full, packet dropped from " MACSTR, MAC2STR(peer_address));
            #endif
        }
    }
}

esp_err_t _add_peer_internal(uint8_t *peer_mac, const char *name, bool is_connected, uint32_t key)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    FPR_STORE_HASH_TYPE *store = (FPR_STORE_HASH_TYPE *)heap_caps_calloc(1, sizeof(FPR_STORE_HASH_TYPE), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(store != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate peer store");
    
    _safe_string_copy(store->name, name ? name : "Unnamed", sizeof(store->name));
    store->response_queue = xQueueCreate(FPR_QUEUE_LENGTH, sizeof(fpr_package_t));
    if (!store->response_queue) {
        heap_caps_free(store);
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
    store->queue_mode = fpr_net.default_queue_mode;
    store->receiving_fragmented = false;
    store->fragment_seq_num = 0;
    
    // Setup peer_info with the actual MAC - this creates a persistent copy
    memcpy(store->peer_info.peer_addr, peer_mac, 6);
    fpr_set_peer_info(&store->peer_info);
    
    // Use the persistent MAC copy (peer_info.peer_addr) as the hashmap key
    // This ensures the key remains valid as long as the peer entry exists
    bool success = hashmap_put(&fpr_net.peers_map, store->peer_info.peer_addr, store);
    if (success) {
        esp_now_del_peer(store->peer_info.peer_addr);
        esp_err_t err = esp_now_add_peer(&store->peer_info);
        if (err != ESP_OK) {
            hashmap_remove(&fpr_net.peers_map, store->peer_info.peer_addr);
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

// used by multiple modes
esp_err_t _add_discovered_peer(const char *name, uint8_t *address, uint32_t key, bool is_connected) 
{
    ESP_RETURN_ON_FALSE(name != NULL && address != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    return _add_peer_internal(address, name, is_connected, key);
}

// Helper: Copy peer data to info structure
void _copy_peer_to_info(const FPR_STORE_HASH_TYPE *peer, fpr_peer_info_t *info)
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