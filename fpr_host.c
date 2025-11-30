#include "fpr/fpr_host.h"
#include "fpr/fpr_security.h"
#include "fpr/fpr_security_handshake.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "fpr_host";

extern esp_err_t fpr_network_send_device_info(uint8_t *peer_address);
extern fpr_connect_t make_fpr_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk);
extern esp_err_t fpr_network_send_to_peer(uint8_t *peer_address, void *data, int size, fpr_package_id_t package_id);

static void _count_connected_callback(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    size_t *count = (size_t *)user_data;
    if (peer && peer->state == FPR_PEER_STATE_CONNECTED) {
        (*count)++;
    }
}

size_t fpr_host_get_connected_count()
{
    size_t count = 0;
    hashmap_foreach(&fpr_net.peers_map, _count_connected_callback, &count);
    return count;
}

static bool _allow_peer_to_connect(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing) 
{
    // Check if peer is blocked
    if (existing && existing->state == FPR_PEER_STATE_BLOCKED) {
        ESP_LOGW(TAG, "Peer " MACSTR " is blocked - ignoring request", MAC2STR(esp_now_info->src_addr));
        return false;
    }

    // Verify PWK if included (skip verification for initial discovery)
    if (info->has_pwk && fpr_net.host_pwk_valid) {
        if (!fpr_security_verify_pwk(info->pwk, fpr_net.host_pwk)) {
            ESP_LOGW(TAG, "Invalid PWK from " MACSTR, MAC2STR(esp_now_info->src_addr));
            return false;
        }
    }
    
    // Check max peers limit
    if (fpr_net.host_config.max_peers > 0) {
        size_t connected_count = fpr_host_get_connected_count();
        if (connected_count >= fpr_net.host_config.max_peers && (!existing || existing->state != FPR_PEER_STATE_CONNECTED)) {
            ESP_LOGW(TAG, "Max peers limit reached (%zu/%d) - rejecting %s", 
                     connected_count, fpr_net.host_config.max_peers, info->name);
            return false;
        }
    }
    return true;
}

static void _handle_host_auto_mode(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing) 
{
    if (existing && existing->is_connected) {
        // Check if client is requesting reconnection (client restarted and lost keys)
        if (!info->has_pwk && !info->has_lwk) {
            ESP_LOGI(TAG, "Client %s reconnecting (restarted) - reinitiating handshake", existing->name);
            // Client restarted - reinitiate handshake
            existing->is_connected = false;
            existing->state = FPR_PEER_STATE_DISCOVERED;
            existing->sec_state = FPR_SEC_STATE_NONE;
            existing->security.pwk_valid = false;
            existing->security.lwk_valid = false;
            _update_peer_rssi_and_timestamp(existing, esp_now_info);
            // Send PWK to restart handshake
            fpr_sec_host_send_pwk(esp_now_info->src_addr, existing, fpr_net.host_pwk);
        } else {
            // Already connected, just update timestamp
            _update_peer_rssi_and_timestamp(existing, esp_now_info);
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Peer already connected: %s", existing->name);
            #endif
        }
    } else {
        // Add or update peer
        esp_err_t err = ESP_OK;
        if (existing) {
            _update_peer_rssi_and_timestamp(existing, esp_now_info);
        } else {
            err = _add_discovered_peer(info->name, esp_now_info->src_addr, 0, false);
            existing = _get_peer_from_map(esp_now_info->src_addr);
        }
        
        if (err == ESP_OK && existing) {
            // Handle security state machine (WiFi-style)
            if (!info->has_pwk) {
                // Step 1: Client sent initial request without PWK
                // Send back device info with PWK
                fpr_sec_host_send_pwk(esp_now_info->src_addr, existing, fpr_net.host_pwk);
            } else if (info->has_pwk && info->has_lwk) {
                // Step 3: Client sent PWK + its own LWK
                // Verify PWK, store client's LWK, send acknowledgment, mark connected
                fpr_sec_host_verify_and_ack(esp_now_info->src_addr, existing, info, fpr_net.host_pwk);
            }
        } else {
            ESP_LOGE(TAG, "Failed to add peer: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t fpr_host_approve_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    if (peer->state == FPR_PEER_STATE_BLOCKED) {
        ESP_LOGW(TAG, "Cannot approve blocked peer " MACSTR, MAC2STR(peer_mac));
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check max peers limit
    if (fpr_net.host_config.max_peers > 0) {
        size_t connected_count = fpr_host_get_connected_count();
        if (connected_count >= fpr_net.host_config.max_peers && peer->state != FPR_PEER_STATE_CONNECTED) {
            ESP_LOGW(TAG, "Max peers limit reached - cannot approve");
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "Peer approved: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    
    // Initiate security handshake by sending PWK
    esp_err_t err;
    if (fpr_net.host_pwk_valid) {
        // Send PWK to start handshake
        err = fpr_sec_host_send_pwk(peer_mac, peer, fpr_net.host_pwk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send PWK to approved peer: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Sent PWK to approved client - waiting for handshake completion");
    } else {
        // No security - mark as connected immediately (legacy mode)
        peer->is_connected = true;
        peer->state = FPR_PEER_STATE_CONNECTED;
        err = fpr_network_send_device_info(peer_mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send approval to peer: %s", esp_err_to_name(err));
        }
    }
    
    return err;
}

esp_err_t fpr_host_reject_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    peer->is_connected = false;
    peer->state = FPR_PEER_STATE_REJECTED;
    ESP_LOGI(TAG, "Peer rejected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    
    return ESP_OK;
}

static void _handle_host_manual_mode(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing)
{
    if (existing) {
        // Check if client is requesting reconnection after restart (lost keys)
        if (existing->is_connected && !info->has_pwk && !info->has_lwk) {
            ESP_LOGI(TAG, "Client %s reconnecting (restarted) - resetting for manual approval", existing->name);
            // Reset connection and security state for re-approval
            existing->is_connected = false;
            existing->state = FPR_PEER_STATE_PENDING;
            existing->sec_state = FPR_SEC_STATE_NONE;
            existing->security.pwk_valid = false;
            existing->security.lwk_valid = false;
        } else if (existing->state != FPR_PEER_STATE_CONNECTED) {
            existing->state = FPR_PEER_STATE_PENDING;
        }
        _update_peer_rssi_and_timestamp(existing, esp_now_info);
        _safe_string_copy(existing->name, info->name, sizeof(existing->name));
    } else {
        // Add new peer as pending
        esp_err_t err = _add_discovered_peer(info->name, esp_now_info->src_addr, 0, false);
        if (err == ESP_OK) {
            existing = _get_peer_from_map(esp_now_info->src_addr);
            if (existing) {
                existing->state = FPR_PEER_STATE_PENDING;
            }
        }
    }
    
    ESP_LOGI(TAG, "Connection request from %s - pending manual approval", info->name);
    
    // Invoke approval callback if registered
    if (fpr_net.host_config.request_cb) {
        bool approved = fpr_net.host_config.request_cb(esp_now_info->src_addr, info->name, 0);
        if (approved) {
            fpr_host_approve_peer(esp_now_info->src_addr);
        } else {
            fpr_host_reject_peer(esp_now_info->src_addr);
        }
    }
}

void _handle_host_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    #if (FPR_DEBUG == 1)
    ESP_LOGI(TAG, "Host received packet - len: %d, from: " MACSTR ", to: " MACSTR, 
             len, MAC2STR(esp_now_info->src_addr), MAC2STR(esp_now_info->des_addr));
    #endif
    
    // Check if network is paused
    if (fpr_net.paused) {
        return;  // Drop all packets when paused
    }
    
    if (!is_fpr_package_compatible(len)) {
        ESP_LOGW(TAG, "Packet size mismatch - expected: %d, got: %d", sizeof(fpr_package_t), len);
        return;
    }
    
    fpr_package_t *package = (fpr_package_t *)data;
    
    // Version handling (using fpr_handle.h)
    if (!fpr_version_handle_version(esp_now_info, data, len, package->version)) {
        return; // Version handler rejected the packet
    }
    
    bool is_broadcast = is_broadcast_address(esp_now_info->des_addr);
    
    #if (FPR_DEBUG == 1)
    ESP_LOGI(TAG, "Packet is %s, package_type: %d", is_broadcast ? "BROADCAST" : "UNICAST", package->package_type);
    #endif
    
    fpr_connect_t *info = &package->protocol.connect_info;
    
    // Handle unicast messages from clients (connection requests)
    if (!is_broadcast) {
        FPR_STORE_HASH_TYPE *existing = _get_peer_from_map(esp_now_info->src_addr);

        // Check if this is a connection request (peer doesn't exist, not connected, or reconnecting after restart)
        // Client reconnection detected: existing connection but client sends request without keys (client restarted)
        bool is_reconnection = (existing && existing->is_connected && !info->has_pwk && !info->has_lwk);
        bool is_connection_request = (!existing || !existing->is_connected || is_reconnection);
        
        if (is_connection_request) {
            if (is_reconnection) {
                ESP_LOGI(TAG, "Client %s reconnecting after restart", existing->name);
            } else {
                ESP_LOGI(TAG, "Processing connection request from %s, visibility: %d", 
                    info->name, info->visibility);
            }

            if (!_allow_peer_to_connect(esp_now_info, info, existing)) {
                ESP_LOGW(TAG, "Connection from %s denied", info->name);
                return;
            }
            
            // Handle based on connection mode
            if (fpr_net.host_config.connection_mode == FPR_CONNECTION_AUTO) {
                // Auto mode - immediately accept connection
                _handle_host_auto_mode(esp_now_info, info, existing);
            } else {
                // Manual mode - handle connection approval
                _handle_host_manual_mode(esp_now_info, info, existing);
            }
        }
        else {
            // Peer already connected - update timestamp and store application data
            _update_peer_rssi_and_timestamp(existing, esp_now_info);
            ESP_LOGI(TAG, "Received packet from connected peer: %s", existing->name);
            _store_data_from_peer_helper(esp_now_info, (const fpr_package_t *)data);
        }
    }
}

esp_err_t fpr_host_block_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    
    if (peer) {
        peer->is_connected = false;
        peer->state = FPR_PEER_STATE_BLOCKED;
        ESP_LOGI(TAG, "Peer blocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    } else {
        // Add as blocked even if not in map yet
        esp_err_t err = _add_peer_internal(peer_mac, "Blocked", false, 0);
        if (err == ESP_OK) {
            peer = _get_peer_from_map(peer_mac);
            if (peer) {
                peer->state = FPR_PEER_STATE_BLOCKED;
                ESP_LOGI(TAG, "Peer blocked: " MACSTR, MAC2STR(peer_mac));
            }
        }
        return err;
    }
    
    return ESP_OK;
}

esp_err_t fpr_host_unblock_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    if (peer->state == FPR_PEER_STATE_BLOCKED) {
        peer->state = FPR_PEER_STATE_DISCOVERED;
        ESP_LOGI(TAG, "Peer unblocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_STATE;
}

esp_err_t fpr_host_disconnect_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    peer->is_connected = false;
    peer->state = FPR_PEER_STATE_DISCOVERED;
    ESP_LOGI(TAG, "Peer disconnected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    
    return ESP_OK;
}

static void _host_reconnect_and_keepalive_cb(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    if (!peer) return;

    if (peer->state == FPR_PEER_STATE_CONNECTED) {
        int64_t age_us = esp_timer_get_time() - peer->last_seen;
        uint64_t age_ms = (uint64_t)US_TO_MS(age_us);
        if (age_ms > FPR_RECONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Client " MACSTR " timed out (age %llu ms) - disconnecting", MAC2STR(peer->peer_info.peer_addr), age_ms);
            peer->is_connected = false;
            peer->state = FPR_PEER_STATE_DISCOVERED;
            return;
        }

        // Send keepalive (device info) to client to prompt a response
        esp_err_t err = fpr_network_send_device_info(peer->peer_info.peer_addr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Host keepalive send to " MACSTR " failed: %s", MAC2STR(peer->peer_info.peer_addr), esp_err_to_name(err));
        }
    }
}

void _fpr_host_reconnect_task(void *arg)
{
    (void)arg;
    TickType_t last_keep = xTaskGetTickCount();
    const TickType_t keep_interval_ticks = pdMS_TO_TICKS(FPR_KEEPALIVE_INTERVAL_MS);
    const TickType_t check_interval_ticks = pdMS_TO_TICKS(FPR_HOST_SCAN_POLL_INTERVAL_MS);

    while (1) {
        // Periodically run keepalive + reconnect checks for connected clients
        if ((xTaskGetTickCount() - last_keep) >= keep_interval_ticks) {
            hashmap_foreach(&fpr_net.peers_map, _host_reconnect_and_keepalive_cb, NULL);
            last_keep = xTaskGetTickCount();
        }

        vTaskDelay(check_interval_ticks);
    }
}

esp_err_t fpr_host_set_config(const fpr_host_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    
    fpr_net.host_config = *config;
    ESP_LOGI(TAG, "Host config updated: max_peers=%d, mode=%s", 
             config->max_peers, 
             config->connection_mode == FPR_CONNECTION_AUTO ? "AUTO" : "MANUAL");
    return ESP_OK;
}

esp_err_t fpr_host_get_config(fpr_host_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    
    *config = fpr_net.host_config;
    return ESP_OK;
}