/**
 * @file sprout_host.c
 * @brief FPR Host Mode Implementation
 * 
 * Implements host mode functionality including client discovery,
 * connection management, and the host side of the security handshake.
 * 
 * @version 1.0.0 (Stable)
 * @date December 2025S
 */

#include "sprout/sprout_host.h"
#include "sprout/sprout_security.h"
#include "sprout/sprout_security_handshake.h"
#include "sprout/sprout.h"
#include "sprout/internal/helpers.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "sprout_host";

static void _count_connected_callback(void *key, void *value, void *user_data)
{
    (void)key;
    SPROUT_STORE_HASH_TYPE *peer = (SPROUT_STORE_HASH_TYPE *)value;
    size_t *count = (size_t *)user_data;
    if (peer && peer->state == SPROUT_PEER_STATE_CONNECTED) {
        (*count)++;
    }
}

size_t sprout_host_get_connected_count()
{
    size_t count = 0;
    hashmap_foreach(sprout_net.peers_map, _count_connected_callback, &count);
    return count;
}

static bool _allow_peer_to_connect(const esp_now_recv_info_t *esp_now_info, const sprout_connect_t *info, SPROUT_STORE_HASH_TYPE *existing) 
{
    // Check if peer is blocked
    if (existing && existing->state == SPROUT_PEER_STATE_BLOCKED) {
        ESP_LOGW(TAG, "Peer " MACSTR " is blocked - ignoring request", MAC2STR(esp_now_info->src_addr));
        return false;
    }

    // Check allowlist if enabled
    if (sprout_net.allowlist_enabled && sprout_net.allowlist_map) {
        bool is_allowed = hashmap_get(sprout_net.allowlist_map, esp_now_info->src_addr) != NULL;
        if (!is_allowed) {
            ESP_LOGW(TAG, "Peer " MACSTR " not in allowlist - rejecting connection", MAC2STR(esp_now_info->src_addr));
            return false;
        }
    }

    // Verify PWK if included (skip verification for initial discovery)
    if (info->has_pwk && sprout_net.host_pwk_valid) {
        if (!sprout_security_verify_pwk(info->pwk, sprout_net.host_pwk)) {
            ESP_LOGW(TAG, "Invalid PWK from " MACSTR, MAC2STR(esp_now_info->src_addr));
            return false;
        }
    }
    
    // Check max peers limit
    if (sprout_net.host_config.max_peers > 0) {
        size_t connected_count = sprout_host_get_connected_count();
        if (connected_count >= sprout_net.host_config.max_peers && (!existing || existing->state != SPROUT_PEER_STATE_CONNECTED)) {
            ESP_LOGW(TAG, "Max peers limit reached (%zu/%d) - rejecting %s", 
                     connected_count, sprout_net.host_config.max_peers, info->name);
            return false;
        }
    }
    return true;
}

static void _handle_host_auto_mode(const esp_now_recv_info_t *esp_now_info, const sprout_connect_t *info, SPROUT_STORE_HASH_TYPE *existing) 
{
    // Check if client is requesting reconnection (client restarted and lost keys)
    if (existing && existing->is_connected && !info->has_pwk && !info->has_lwk) {
        ESP_LOGI(TAG, "Client %s reconnecting (restarted) - reinitiating handshake", existing->name);
        // Client restarted - reinitiate handshake
        existing->is_connected = false;
        existing->state = SPROUT_PEER_STATE_DISCOVERED;
        existing->sec_state = SPROUT_SEC_STATE_NONE;
        existing->security.pwk_valid = false;
        existing->security.lwk_valid = false;
        _update_peer_rssi_and_timestamp(existing, esp_now_info);
        // Send PWK to restart handshake
        sprout_sec_host_send_pwk(esp_now_info->src_addr, existing, sprout_net.host_pwk);
        return; // Done - wait for client's response
    }
    
    // If already connected and not reconnecting, just update timestamp
    if (existing && existing->is_connected) {
        _update_peer_rssi_and_timestamp(existing, esp_now_info);
        #if (SPROUT_DEBUG == 1)
        ESP_LOGW(TAG, "Peer already connected: %s", existing->name);
        #endif
        return;
    }
    
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
        if (!info->has_pwk && existing->sec_state == SPROUT_SEC_STATE_NONE) {
            // Step 1: Client sent initial request without PWK
            // Send back device info with PWK (only if not already sent)
            sprout_sec_host_send_pwk(esp_now_info->src_addr, existing, sprout_net.host_pwk);
        } else if (info->has_pwk && info->has_lwk) {
            // Step 3: Client sent PWK + its own LWK
            
            // CRITICAL: Handle timing/race conditions gracefully
            // Expected state is PWK_SENT, but allow processing if:
            // - State is PWK_SENT (normal case)
            // - State is NONE but we just restarted (missed our own PWK send)
            // Don't process if already ESTABLISHED (avoid duplicate processing)
            
            if (existing->sec_state == SPROUT_SEC_STATE_ESTABLISHED) {
                #if (SPROUT_DEBUG == 1)
                ESP_LOGD(TAG, "Client %s already established, ignoring duplicate response", existing->name);
                #endif
            } else if (existing->sec_state == SPROUT_SEC_STATE_PWK_SENT || existing->sec_state == SPROUT_SEC_STATE_NONE) {
                // Verify PWK, store client's LWK, send acknowledgment, mark connected
                sprout_sec_host_verify_and_ack(esp_now_info->src_addr, existing, info, sprout_net.host_pwk);
            }
            #if (SPROUT_DEBUG == 1)
            else {
                ESP_LOGW(TAG, "Ignoring client response - unexpected state (current=%d, has_pwk=%d, has_lwk=%d)", 
                         existing->sec_state, info->has_pwk, info->has_lwk);
            }
            #endif
        }
        #if (SPROUT_DEBUG == 1)
        else if (!info->has_pwk && existing->sec_state != SPROUT_SEC_STATE_NONE) {
            ESP_LOGD(TAG, "Ignoring duplicate initial request (state=%d)", existing->sec_state);
        }
        #endif
    } else {
        ESP_LOGE(TAG, "Failed to add peer: %s", esp_err_to_name(err));
    }
}

esp_err_t sprout_host_approve_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    if (peer->state == SPROUT_PEER_STATE_BLOCKED) {
        ESP_LOGW(TAG, "Cannot approve blocked peer " MACSTR, MAC2STR(peer_mac));
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check max peers limit
    if (sprout_net.host_config.max_peers > 0) {
        size_t connected_count = sprout_host_get_connected_count();
        if (connected_count >= sprout_net.host_config.max_peers && peer->state != SPROUT_PEER_STATE_CONNECTED) {
            ESP_LOGW(TAG, "Max peers limit reached - cannot approve");
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "Peer approved: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    
    // Initiate security handshake by sending PWK
    esp_err_t err;
    if (sprout_net.host_pwk_valid) {
        // Send PWK to start handshake
        err = sprout_sec_host_send_pwk(peer_mac, peer, sprout_net.host_pwk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send PWK to approved peer: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Sent PWK to approved client - waiting for handshake completion");
    } else {
        // No security - mark as connected immediately (legacy mode)
        peer->is_connected = true;
        peer->state = SPROUT_PEER_STATE_CONNECTED;
        err = sprout_send_device_info(peer_mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send approval to peer: %s", esp_err_to_name(err));
        }
    }
    
    return err;
}

esp_err_t sprout_host_reject_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    peer->is_connected = false;
    peer->state = SPROUT_PEER_STATE_REJECTED;
    ESP_LOGI(TAG, "Peer rejected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    
    return ESP_OK;
}

static void _handle_host_manual_mode(const esp_now_recv_info_t *esp_now_info, const sprout_connect_t *info, SPROUT_STORE_HASH_TYPE *existing)
{
    if (existing) {
        // Check if client is requesting reconnection after restart (lost keys)
        if (existing->is_connected && !info->has_pwk && !info->has_lwk) {
            ESP_LOGI(TAG, "Client %s reconnecting (restarted) - resetting for manual approval", existing->name);
            // Reset connection and security state for re-approval
            existing->is_connected = false;
            existing->state = SPROUT_PEER_STATE_PENDING;
            existing->sec_state = SPROUT_SEC_STATE_NONE;
            existing->security.pwk_valid = false;
            existing->security.lwk_valid = false;
        } else if (existing->state != SPROUT_PEER_STATE_CONNECTED) {
            existing->state = SPROUT_PEER_STATE_PENDING;
        }
        _update_peer_rssi_and_timestamp(existing, esp_now_info);
        _safe_string_copy(existing->name, info->name, sizeof(existing->name));
    } else {
        // Add new peer as pending
        esp_err_t err = _add_discovered_peer(info->name, esp_now_info->src_addr, 0, false);
        if (err == ESP_OK) {
            existing = _get_peer_from_map(esp_now_info->src_addr);
            if (existing) {
                existing->state = SPROUT_PEER_STATE_PENDING;
            }
        }
    }
    
    ESP_LOGI(TAG, "Connection request from %s - pending manual approval", info->name);
    
    // Invoke approval callback if registered
    if (sprout_net.host_config.request_cb) {
        bool approved = sprout_net.host_config.request_cb(esp_now_info->src_addr, info->name, 0);
        if (approved) {
            sprout_host_approve_peer(esp_now_info->src_addr);
        } else {
            sprout_host_reject_peer(esp_now_info->src_addr);
        }
    }
}

void _handle_host_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    #if (SPROUT_DEBUG_LOG_HOST_DATA_RECEIVE == 1)
    ESP_LOGI(TAG, "Host received packet - len: %d, from: " MACSTR ", to: " MACSTR, 
             len, MAC2STR(esp_now_info->src_addr), MAC2STR(esp_now_info->des_addr));
    #endif
    
    // Check if network is paused
    if (sprout_net.paused) {
        return;  // Drop all packets when paused
    }
    
    if (!is_sprout_package_compatible(len)) {
        ESP_LOGW(TAG, "Packet size mismatch - expected: %d, got: %d", sizeof(sprout_package_t), len);
        return;
    }
    
    sprout_package_t *package = (sprout_package_t *)data;
    
    // Version handling (using sprout_handle.h)
    if (!sprout_version_handle_version(esp_now_info, data, len, package->version)) {
        return; // Version handler rejected the packet
    }
    
    bool is_broadcast = is_broadcast_address(esp_now_info->des_addr);
    
    #if (SPROUT_DEBUG == 1)
    ESP_LOGI(TAG, "Packet is %s, package_type: %d, id: %d", is_broadcast ? "BROADCAST" : "UNICAST", package->package_type, package->id);
    #endif
    
    // Connection/handshake packets use SPROUT_PACKET_ID_CONTROL (-1)
    // Application data packets use other IDs (0 or positive)
    bool is_control_packet = (package->id == SPROUT_PACKET_ID_CONTROL);
    
    sprout_connect_t *info = &package->protocol.connect_info;
    
    // Handle unicast messages from clients (connection requests)
    if (!is_broadcast) {
        SPROUT_STORE_HASH_TYPE *existing = _get_peer_from_map(esp_now_info->src_addr);

        // Only treat as connection request if:
        // 1. It's a control packet (id == -1), AND
        // 2. Peer doesn't exist, or not connected, or reconnecting after restart
        bool is_reconnection = (existing && existing->is_connected && is_control_packet && !info->has_pwk && !info->has_lwk);
        bool is_connection_request = is_control_packet && (!existing || !existing->is_connected || is_reconnection);
        
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
            if (sprout_net.host_config.connection_mode == SPROUT_CONNECTION_AUTO) {
                // Auto mode - immediately accept connection
                _handle_host_auto_mode(esp_now_info, info, existing);
            } else {
                // Manual mode - handle connection approval
                _handle_host_manual_mode(esp_now_info, info, existing);
            }
        }
        else if (existing && existing->is_connected) {
            // Peer already connected - update timestamp and store application data
            _update_peer_rssi_and_timestamp(existing, esp_now_info);
            #if (SPROUT_DEBUG == 1)
            ESP_LOGI(TAG, "Received packet from connected peer: %s (id: %d)", existing->name, package->id);
            #endif
            _store_data_from_peer_helper(esp_now_info, (const sprout_package_t *)data);
        }
        else {
            // Data packet from unknown/disconnected peer - drop it
            #if (SPROUT_DEBUG == 1)
            ESP_LOGW(TAG, "Dropping data packet (id: %d) from unknown/disconnected peer " MACSTR, 
                     package->id, MAC2STR(esp_now_info->src_addr));
            #endif
        }
    }
}

esp_err_t sprout_host_block_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    
    if (peer) {
        peer->is_connected = false;
        peer->state = SPROUT_PEER_STATE_BLOCKED;
        ESP_LOGI(TAG, "Peer blocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    } else {
        // Add as blocked even if not in map yet
        esp_err_t err = _add_peer_internal(peer_mac, "Blocked", false, 0);
        if (err == ESP_OK) {
            peer = _get_peer_from_map(peer_mac);
            if (peer) {
                peer->state = SPROUT_PEER_STATE_BLOCKED;
                ESP_LOGI(TAG, "Peer blocked: " MACSTR, MAC2STR(peer_mac));
            }
        }
        return err;
    }
    
    return ESP_OK;
}

esp_err_t sprout_host_unblock_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    if (peer->state == SPROUT_PEER_STATE_BLOCKED) {
        peer->state = SPROUT_PEER_STATE_DISCOVERED;
        ESP_LOGI(TAG, "Peer unblocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_STATE;
}

esp_err_t sprout_host_disconnect_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Peer not found");
    
    peer->is_connected = false;
    peer->state = SPROUT_PEER_STATE_DISCOVERED;
    ESP_LOGI(TAG, "Peer disconnected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    
    return ESP_OK;
}

esp_err_t sprout_host_allow_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    if (!sprout_net.allowlist_map) {
        sprout_net.allowlist_map = malloc(sizeof(struct hashmap));
        if (!sprout_net.allowlist_map) {
            return ESP_ERR_NO_MEM;
        }
        hashmap_init(sprout_net.allowlist_map, 16, mac_hash, mac_equals);
    }
    
    // Allocate and store MAC address in allowlist
    uint8_t *mac_copy = malloc(6);
    if (!mac_copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(mac_copy, peer_mac, 6);
    
    bool added = hashmap_put(sprout_net.allowlist_map, mac_copy, mac_copy);
    if (added) {
        ESP_LOGI(TAG, "Peer added to allowlist: " MACSTR, MAC2STR(peer_mac));
        return ESP_OK;
    } else {
        free(mac_copy);
        return ESP_ERR_INVALID_STATE;  // Already in allowlist
    }
}

esp_err_t sprout_host_disallow_peer(uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    ESP_RETURN_ON_FALSE(sprout_net.allowlist_map != NULL, ESP_ERR_INVALID_STATE, TAG, "Allowlist not initialized");
    
    bool removed = hashmap_remove(sprout_net.allowlist_map, peer_mac);
    if (removed) {
        ESP_LOGI(TAG, "Peer removed from allowlist: " MACSTR, MAC2STR(peer_mac));
        return ESP_OK;
    } else {
        return ESP_ERR_NOT_FOUND;  // Not in allowlist
    }
}

esp_err_t sprout_host_enable_allowlist(void)
{
    if (!sprout_net.allowlist_map) {
        sprout_net.allowlist_map = malloc(sizeof(struct hashmap));
        if (!sprout_net.allowlist_map) {
            return ESP_ERR_NO_MEM;
        }
        hashmap_init(sprout_net.allowlist_map, 16, mac_hash, mac_equals);
    }
    
    sprout_net.allowlist_enabled = true;
    ESP_LOGI(TAG, "Allowlist filtering enabled");
    return ESP_OK;
}

esp_err_t sprout_host_disable_allowlist(void)
{
    sprout_net.allowlist_enabled = false;
    ESP_LOGI(TAG, "Allowlist filtering disabled");
    return ESP_OK;
}

bool sprout_host_is_allowlist_enabled(void)
{
    return sprout_net.allowlist_enabled;
}

static void _host_reconnect_and_keepalive_cb(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    SPROUT_STORE_HASH_TYPE *peer = (SPROUT_STORE_HASH_TYPE *)value;
    if (!peer) return;

    if (peer->state == SPROUT_PEER_STATE_CONNECTED) {
        int64_t age_us = esp_timer_get_time() - peer->last_seen;
        uint64_t age_ms = (uint64_t)US_TO_MS(age_us);
        if (age_ms > SPROUT_RECONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Client " MACSTR " timed out (age %llu ms) - disconnecting", MAC2STR(peer->peer_info.peer_addr), age_ms);
            peer->is_connected = false;
            peer->state = SPROUT_PEER_STATE_DISCOVERED;
            return;
        }

        // Send keepalive (device info) to client to prompt a response
        esp_err_t err = sprout_send_device_info(peer->peer_info.peer_addr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Host keepalive send to " MACSTR " failed: %s", MAC2STR(peer->peer_info.peer_addr), esp_err_to_name(err));
        }
    }
}

void _host_reconnect_task(void *arg)
{
    (void)arg;
    TickType_t last_keep = xTaskGetTickCount();

    while (1) {
        // Get power-adjusted intervals
        const TickType_t keep_interval_ticks = pdMS_TO_TICKS(_sprout_get_power_adjusted_interval(SPROUT_KEEPALIVE_INTERVAL_MS));
        const TickType_t check_interval_ticks = pdMS_TO_TICKS(_sprout_get_power_adjusted_interval(SPROUT_HOST_SCAN_POLL_INTERVAL_MS));
        
        // Periodically run keepalive + reconnect checks for connected clients
        if ((xTaskGetTickCount() - last_keep) >= keep_interval_ticks) {
            hashmap_foreach(sprout_net.peers_map, _host_reconnect_and_keepalive_cb, NULL);
            last_keep = xTaskGetTickCount();
        }

        vTaskDelay(check_interval_ticks);
    }
}

esp_err_t sprout_host_set_max_peers(uint8_t max_peers)
{
    sprout_net.host_config.max_peers = max_peers;
    ESP_LOGI(TAG, "Host max peers set to %d", max_peers);
    return ESP_OK;
}

esp_err_t sprout_host_set_connection_mode(sprout_connection_mode_t mode)
{
    sprout_net.host_config.connection_mode = mode;
    ESP_LOGI(TAG, "Host connection mode set to %s", mode == SPROUT_CONNECTION_AUTO ? "AUTO" : "MANUAL");
    return ESP_OK;
}

void sprout_host_register_request_callback(sprout_connection_request_cb_t cb)
{
    sprout_net.host_config.request_cb = cb;
    ESP_LOGI(TAG, "Host request callback %s", cb ? "registered" : "unregistered");
}