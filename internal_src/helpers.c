#include "fpr/internal/helpers.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "fpr_helpers";

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
        
        // Store in queue
        // data should be of type fpr_package_t
        xQueueSend(store->response_queue, (void*)data, pdMS_TO_TICKS(FPR_QUEUE_SEND_TIMEOUT_MS));
        
        // Call application callback if registered
        if (fpr_net.data_callback) {
            fpr_package_t *package = (fpr_package_t *)data;
            // Pass the protocol payload size to the application callback
            int data_len = (int)sizeof(package->protocol);
            fpr_net.data_callback(peer_address, &package->protocol, &data_len);
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
    
    // Setup peer_info with the actual MAC
    memcpy(store->peer_info.peer_addr, peer_mac, 6);
    fpr_set_peer_info(&store->peer_info);
    
    bool success = hashmap_put(&fpr_net.peers_map, peer_mac, store);
    if (success) {
        esp_now_del_peer(peer_mac);
        esp_err_t err = esp_now_add_peer(&store->peer_info);
        if (err != ESP_OK) {
            hashmap_remove(&fpr_net.peers_map, peer_mac);
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