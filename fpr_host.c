/**
 * @file fpr_host.c
 * @brief FPR Host Mode Implementation
 * 
 * Implements host mode functionality including client discovery,
 * connection management, and the host side of the security handshake.
 * 
 * @version 1.0.0 (Stable)
 * @date December 2025S
 */

#include "fpr/fpr.h"
#include "fpr/internal/rx.h"
#include "fpr/fpr_security.h"
#include "fpr/internal/fpr_security_handshake.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "fpr_host";

static void _peer_connected_count_cb(void *key, void *value, void *user_data)
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
    _peers_lock();
    size_t count = 0;
    hashmap_foreach(&fpr_net.peers_map, _peer_connected_count_cb, &count);
    _peers_unlock();
    return count;
}

static bool _host_allow_connect(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing) 
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

static void _host_rx_auto(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing) 
{
    // Check if client is requesting reconnection (client restarted and lost keys)
    if (existing && existing->is_connected && !info->has_pwk && !info->has_lwk) {
        ESP_LOGI(TAG, "Client %s reconnecting (restarted) - reinitiating handshake", existing->name);
        // Client restarted - reinitiate handshake
        _reset_peer_connection(existing, FPR_PEER_STATE_DISCOVERED);
        _peer_touch(existing, esp_now_info);
        // Send PWK to restart handshake
        fpr_sec_host_send_pwk(esp_now_info->src_addr, existing, fpr_net.host_pwk);
        return; // Done - wait for client's response
    }
    
    // If already connected and not reconnecting, just update timestamp
    if (existing && existing->is_connected) {
        _peer_touch(existing, esp_now_info);
        #if (FPR_DEBUG == 1)
        ESP_LOGW(TAG, "Peer already connected: %s", existing->name);
        #endif
        return;
    }
    
    // Add or update peer
    esp_err_t err = ESP_OK;
    if (existing) {
        _peer_touch(existing, esp_now_info);
    } else {
        err = _peer_add_discovered(info->name, esp_now_info->src_addr, 0, false);
        existing = _peer_from_map(esp_now_info->src_addr);
    }
    
    if (err == ESP_OK && existing) {
        // Handle security state machine (WiFi-style)
        if (!info->has_pwk && existing->sec_state == FPR_SEC_STATE_NONE) {
            // Step 1: Client sent initial request without PWK
            // Send back device info with PWK (only if not already sent)
            fpr_sec_host_send_pwk(esp_now_info->src_addr, existing, fpr_net.host_pwk);
        } else if (info->has_pwk && info->has_lwk) {
            // Step 3: Client sent PWK + its own LWK
            
            // CRITICAL: Handle timing/race conditions gracefully
            // Expected state is PWK_SENT, but allow processing if:
            // - State is PWK_SENT (normal case)
            // - State is NONE but we just restarted (missed our own PWK send)
            // Don't process if already ESTABLISHED (avoid duplicate processing)
            
            if (existing->sec_state == FPR_SEC_STATE_ESTABLISHED) {
                #if (FPR_DEBUG == 1)
                ESP_LOGD(TAG, "Client %s already established, ignoring duplicate response", existing->name);
                #endif
            } else if (existing->sec_state == FPR_SEC_STATE_PWK_SENT || existing->sec_state == FPR_SEC_STATE_NONE) {
                // Verify PWK, store client's LWK, send acknowledgment, mark connected
                fpr_sec_host_verify_and_ack(esp_now_info->src_addr, existing, info, fpr_net.host_pwk);
            }
            #if (FPR_DEBUG == 1)
            else {
                ESP_LOGW(TAG, "Ignoring client response - unexpected state (current=%d, has_pwk=%d, has_lwk=%d)", 
                         existing->sec_state, info->has_pwk, info->has_lwk);
            }
            #endif
        }
        #if (FPR_DEBUG == 1)
        else if (!info->has_pwk && existing->sec_state != FPR_SEC_STATE_NONE) {
            ESP_LOGD(TAG, "Ignoring duplicate initial request (state=%d)", existing->sec_state);
        }
        #endif
    } else {
        ESP_LOGE(TAG, "Failed to add peer: %s", esp_err_to_name(err));
    }
}

esp_err_t fpr_host_approve_peer(const uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (peer->state == FPR_PEER_STATE_BLOCKED) {
        _peers_unlock();
        ESP_LOGW(TAG, "Cannot approve blocked peer " MACSTR, MAC2STR(peer_mac));
        return ESP_ERR_INVALID_STATE;
    }

    // Check max peers limit
    if (fpr_net.host_config.max_peers > 0) {
        size_t connected_count = fpr_host_get_connected_count();
        if (connected_count >= fpr_net.host_config.max_peers && peer->state != FPR_PEER_STATE_CONNECTED) {
            _peers_unlock();
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
            _peers_unlock();
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

    _peers_unlock();
    return err;
}

esp_err_t fpr_host_reject_peer(const uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    _reset_peer_connection(peer, FPR_PEER_STATE_REJECTED);
    ESP_LOGI(TAG, "Peer rejected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    _peers_unlock();

    return ESP_OK;
}

static void _host_rx_manual(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing)
{
    /* Handshake completion after approval: the client answered our PWK
     * with PWK+LWK. Finish the handshake without re-running the approval
     * callback — otherwise every reply re-approves and resends the PWK,
     * and the peers livelock before ever connecting. */
    if (existing && existing->sec_state == FPR_SEC_STATE_PWK_SENT &&
        info->has_pwk && info->has_lwk) {
        _peer_touch(existing, esp_now_info);
        if (fpr_net.host_pwk_valid) {
            fpr_sec_host_verify_and_ack(esp_now_info->src_addr, existing, info, fpr_net.host_pwk);
        }
        return;
    }

    if (existing) {
        // Check if client is requesting reconnection after restart (lost keys)
        if (existing->is_connected && !info->has_pwk && !info->has_lwk) {
            ESP_LOGI(TAG, "Client %s reconnecting (restarted) - resetting for manual approval", existing->name);
            // Reset connection and security state for re-approval
            _reset_peer_connection(existing, FPR_PEER_STATE_PENDING);
        } else if (existing->state != FPR_PEER_STATE_CONNECTED) {
            existing->state = FPR_PEER_STATE_PENDING;
        }
        _peer_touch(existing, esp_now_info);
        _safe_string_copy(existing->name, info->name, sizeof(existing->name));
    } else {
        // Add new peer as pending
        esp_err_t err = _peer_add_discovered(info->name, esp_now_info->src_addr, 0, false);
        if (err == ESP_OK) {
            existing = _peer_from_map(esp_now_info->src_addr);
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

void _rx_host(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    #if (FPR_DEBUG_LOG_HOST_DATA_RECEIVE == 1)
    ESP_LOGI(TAG, "Host received packet - len: %d, from: " MACSTR ", to: " MACSTR, 
             len, MAC2STR(esp_now_info->src_addr), MAC2STR(esp_now_info->des_addr));
    #endif
    
    /* paused/size/version gates, control sanitize, peers_lock + quiesce
     * re-check — shared preamble, returns with the lock held. */
    fpr_rx_ctx_t ctx;
    if (!_rx_begin(esp_now_info, data, len, false, &ctx)) {
        return;
    }
    const fpr_package_t *package = ctx.package;
    const fpr_connect_t *info = &ctx.control;
    bool is_control_packet = ctx.is_control;
    bool is_broadcast = ctx.is_broadcast;

    #if (FPR_DEBUG == 1)
    ESP_LOGI(TAG, "Packet is %s, package_type: %d, id: %d", is_broadcast ? "BROADCAST" : "UNICAST", package->package_type, package->id);
    #endif

    // Handle unicast messages from clients (connection requests)
    if (!is_broadcast) {
        FPR_STORE_HASH_TYPE *existing = _peer_from_map(esp_now_info->src_addr);

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

            if (!_host_allow_connect(esp_now_info, info, existing)) {
                _peers_unlock();
                ESP_LOGW(TAG, "Connection from %s denied", info->name);
                return;
            }
            
            // Handle based on connection mode
            if (fpr_net.host_config.connection_mode == FPR_CONNECTION_AUTO) {
                // Auto mode - immediately accept connection
                _host_rx_auto(esp_now_info, info, existing);
            } else {
                // Manual mode - handle connection approval
                _host_rx_manual(esp_now_info, info, existing);
            }
        }
        else if (existing && existing->is_connected) {
            // Peer already connected - update timestamp and store application data
            _peer_touch(existing, esp_now_info);
            #if (FPR_DEBUG == 1)
            ESP_LOGI(TAG, "Received packet from connected peer: %s (id: %d)", existing->name, package->id);
            #endif
            _peer_store_data(esp_now_info, (const fpr_package_t *)data);
        }
        else {
            // Data packet from unknown/disconnected peer - drop it
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Dropping data packet (id: %d) from unknown/disconnected peer " MACSTR,
                     package->id, MAC2STR(esp_now_info->src_addr));
            #endif
        }
    }
    _peers_unlock();
}

esp_err_t fpr_host_block_peer(const uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);

    if (peer) {
        peer->is_connected = false;
        peer->state = FPR_PEER_STATE_BLOCKED;
        ESP_LOGI(TAG, "Peer blocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
        _peers_unlock();
    } else {
        // Add as blocked even if not in map yet
        esp_err_t err = _peer_add(peer_mac, "Blocked", false, 0);
        if (err == ESP_OK) {
            peer = _peer_from_map(peer_mac);
            if (peer) {
                peer->state = FPR_PEER_STATE_BLOCKED;
                ESP_LOGI(TAG, "Peer blocked: " MACSTR, MAC2STR(peer_mac));
            }
        }
        _peers_unlock();
        return err;
    }

    return ESP_OK;
}

esp_err_t fpr_host_unblock_peer(const uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (peer->state == FPR_PEER_STATE_BLOCKED) {
        peer->state = FPR_PEER_STATE_DISCOVERED;
        ESP_LOGI(TAG, "Peer unblocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
        _peers_unlock();
        return ESP_OK;
    }
    _peers_unlock();

    return ESP_ERR_INVALID_STATE;
}

esp_err_t fpr_host_disconnect_peer(const uint8_t *peer_mac)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    _reset_peer_connection(peer, FPR_PEER_STATE_DISCOVERED);
    ESP_LOGI(TAG, "Peer disconnected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    _peers_unlock();

    return ESP_OK;
}

static void _host_keepalive_cb(void *key, void *value, void *user_data)
{
    (void)key;
    (void)user_data;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    if (!peer) return;

    if (peer->state == FPR_PEER_STATE_CONNECTED) {
        int64_t age_us = esp_timer_get_time() - peer->last_seen;
        uint64_t age_ms = (uint64_t)FPR_US_TO_MS(age_us);
        if (age_ms > FPR_RECONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Client " MACSTR " timed out (age %llu ms) - disconnecting", MAC2STR(peer->peer_info.peer_addr), age_ms);
            _reset_peer_connection(peer, FPR_PEER_STATE_DISCOVERED);
            return;
        }

        // Send keepalive (device info) to client to prompt a response
        esp_err_t err = fpr_network_send_device_info(peer->peer_info.peer_addr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Host keepalive send to " MACSTR " failed: %s", MAC2STR(peer->peer_info.peer_addr), esp_err_to_name(err));
        }
    }
}

void _task_host_reconnect(void *arg)
{
    (void)arg;
    TickType_t last_keep = xTaskGetTickCount();

    while (!fpr_net.reconnect_stop_requested) {
        // Get power-adjusted intervals
        const TickType_t keep_interval_ticks = pdMS_TO_TICKS(_power_interval(FPR_KEEPALIVE_INTERVAL_MS));
        const TickType_t check_interval_ticks = pdMS_TO_TICKS(_power_interval(FPR_HOST_SCAN_POLL_INTERVAL_MS));
        
        // Periodically run keepalive + reconnect checks for connected clients
        if ((xTaskGetTickCount() - last_keep) >= keep_interval_ticks) {
            _peers_lock();
            hashmap_foreach(&fpr_net.peers_map, _host_keepalive_cb, NULL);
            _peers_unlock();
            last_keep = xTaskGetTickCount();
        }

        vTaskDelay(check_interval_ticks);
    }
    _task_exit(&fpr_net.reconnect_task);
}

esp_err_t fpr_host_set_config(const fpr_host_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");

    /* Whole-struct copy under the lock — rx callbacks read these fields
     * (connection_mode, request_cb) while holding it. */
    _peers_lock();
    fpr_net.host_config = *config;
    _peers_unlock();
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