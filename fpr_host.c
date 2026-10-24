/**
 * @file fpr_host.c
 * @brief FPR Host Mode Implementation
 * 
 * Implements host mode functionality including client discovery,
 * connection management, and the host side of the security handshake.
 * 
 * @version 1.0.0 (Stable)
 * @date December 2025
 */

#include "fpr/fpr.h"
#include "fpr/internal/rx.h"
#include "fpr/fpr_security.h"
#include "fpr/internal/fpr_security_handshake.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_check.h"

static const char *TAG = "fpr_host";
#define FPR_REQUIRE_NODE(n) \
    ESP_RETURN_ON_FALSE((n) != NULL, ESP_ERR_INVALID_ARG, TAG, "Node is NULL")

static void _peer_connected_count_cb(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    size_t *count = (size_t *)user_data;
    if (peer && peer->state == FPR_PEER_STATE_CONNECTED) {
        (*count)++;
    }
}

esp_err_t fpr_host_connected_count(fpr_node_t *node, size_t *out)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Output is NULL");
    *out = 0;
    _peers_lock(node);
    hashmap_foreach(&node->peers_map, _peer_connected_count_cb, out);
    _peers_unlock(node);
    return ESP_OK;
}

static bool _host_allow_connect(fpr_node_t *node, const fpr_rx_frame_t *frame, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing) 
{
    // Check if peer is blocked
    if (existing && existing->state == FPR_PEER_STATE_BLOCKED) {
        ESP_LOGW(TAG, "Peer " MACSTR " is blocked - ignoring request", MAC2STR(frame->src_mac));
        return false;
    }

    // Verify PWK if included (skip verification for initial discovery)
    if (info->has_pwk && node->host_pwk_valid) {
        if (!fpr_security_verify_pwk(info->pwk, node->host_pwk)) {
            ESP_LOGW(TAG, "Invalid PWK from " MACSTR, MAC2STR(frame->src_mac));
            return false;
        }
    }
    
    // Check max peers limit
    if (node->host_config.max_peers > 0) {
        size_t connected_count = 0;
        (void)fpr_host_connected_count(node, &connected_count);
        if (connected_count >= node->host_config.max_peers && (!existing || existing->state != FPR_PEER_STATE_CONNECTED)) {
            ESP_LOGW(TAG, "Max peers limit reached (%zu/%d) - rejecting %s", 
                     connected_count, node->host_config.max_peers, info->name);
            return false;
        }
    }
    return true;
}

static void _host_rx_auto(fpr_node_t *node, const fpr_rx_frame_t *frame, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing) 
{
    // Check if client is requesting reconnection (client restarted and lost keys)
    if (existing && existing->is_connected && !info->has_pwk && !info->has_lwk) {
        ESP_LOGI(TAG, "Client %s reconnecting (restarted) - reinitiating handshake", existing->name);
        // Client restarted - reinitiate handshake
        _reset_peer_connection(existing, FPR_PEER_STATE_DISCOVERED);
        _peer_touch(existing, frame);
        // Send PWK to restart handshake
        fpr_sec_host_send_pwk(node, frame->src_mac, existing, node->host_pwk);
        return; // Done - wait for client's response
    }
    
    // If already connected and not reconnecting, just update timestamp
    if (existing && existing->is_connected) {
        _peer_touch(existing, frame);
        #if (FPR_DEBUG == 1)
        ESP_LOGW(TAG, "Peer already connected: %s", existing->name);
        #endif
        return;
    }
    
    // Add or update peer
    esp_err_t err = ESP_OK;
    if (existing) {
        _peer_touch(existing, frame);
    } else {
        err = _peer_add_discovered(node, info->name, frame->src_mac, false);
        existing = _peer_from_map(node, frame->src_mac);
    }
    
    if (err == ESP_OK && existing) {
        // Handle security state machine (WiFi-style)
        if (!info->has_pwk && existing->sec_state == FPR_SEC_STATE_NONE) {
            // Step 1: Client sent initial request without PWK
            // Send back device info with PWK (only if not already sent)
            fpr_sec_host_send_pwk(node, frame->src_mac, existing, node->host_pwk);
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
                fpr_sec_host_verify_and_ack(node, frame->src_mac, existing, info, node->host_pwk);
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

esp_err_t fpr_host_approve(fpr_node_t *node, const uint8_t *peer_mac)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    FPR_STORE_HASH_TYPE *peer = _peer_lookup_locked(node, peer_mac);
    if (peer == NULL) {
        _peers_unlock(node);
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (peer->state == FPR_PEER_STATE_BLOCKED) {
        _peers_unlock(node);
        ESP_LOGW(TAG, "Cannot approve blocked peer " MACSTR, MAC2STR(peer_mac));
        return ESP_ERR_INVALID_STATE;
    }

    // Check max peers limit
    if (node->host_config.max_peers > 0) {
        size_t connected_count = 0;
        (void)fpr_host_connected_count(node, &connected_count);
        if (connected_count >= node->host_config.max_peers && peer->state != FPR_PEER_STATE_CONNECTED) {
            _peers_unlock(node);
            ESP_LOGW(TAG, "Max peers limit reached - cannot approve");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Peer approved: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));

    // Initiate security handshake by sending PWK
    esp_err_t err;
    if (node->host_pwk_valid) {
        // Send PWK to start handshake
        err = fpr_sec_host_send_pwk(node, peer_mac, peer, node->host_pwk);
        if (err != ESP_OK) {
            _peers_unlock(node);
            ESP_LOGE(TAG, "Failed to send PWK to approved peer: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Sent PWK to approved client - waiting for handshake completion");
    } else {
        // No security - mark as connected immediately (legacy mode)
        peer->is_connected = true;
        peer->state = FPR_PEER_STATE_CONNECTED;
        err = fpr_send_device_info(node, peer_mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send approval to peer: %s", esp_err_to_name(err));
        }
    }

    _peers_unlock(node);
    return err;
}

esp_err_t fpr_host_reject(fpr_node_t *node, const uint8_t *peer_mac)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    FPR_STORE_HASH_TYPE *peer = _peer_lookup_locked(node, peer_mac);
    if (peer == NULL) {
        _peers_unlock(node);
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    _reset_peer_connection(peer, FPR_PEER_STATE_REJECTED);
    ESP_LOGI(TAG, "Peer rejected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    _peers_unlock(node);

    return ESP_OK;
}

static void _host_rx_manual(fpr_node_t *node, const fpr_rx_frame_t *frame, const fpr_connect_t *info, FPR_STORE_HASH_TYPE *existing)
{
    /* Handshake completion after approval: the client answered our PWK
     * with PWK+LWK. Finish the handshake without re-running the approval
     * callback — otherwise every reply re-approves and resends the PWK,
     * and the peers livelock before ever connecting. */
    if (existing && existing->sec_state == FPR_SEC_STATE_PWK_SENT &&
        info->has_pwk && info->has_lwk) {
        _peer_touch(existing, frame);
        if (node->host_pwk_valid) {
            fpr_sec_host_verify_and_ack(node, frame->src_mac, existing, info, node->host_pwk);
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
        _peer_touch(existing, frame);
        _safe_string_copy(existing->name, info->name, sizeof(existing->name));
    } else {
        // Add new peer as pending
        esp_err_t err = _peer_add_discovered(node, info->name, frame->src_mac, false);
        if (err == ESP_OK) {
            existing = _peer_from_map(node, frame->src_mac);
            if (existing) {
                existing->state = FPR_PEER_STATE_PENDING;
            }
        }
    }
    
    ESP_LOGI(TAG, "Connection request from %s - pending manual approval", info->name);
    
    // Invoke approval callback if registered
    if (_cb_has_conn_request(node)) {
        bool approved = _cb_conn_request(node, frame->src_mac, info->name, 0);
        if (approved) {
            fpr_host_approve(node, frame->src_mac);
        } else {
            fpr_host_reject(node, frame->src_mac);
        }
    }
}

void _rx_host(fpr_node_t *node, const fpr_rx_frame_t *frame, const uint8_t *data, int len)
{
    #if (FPR_DEBUG_LOG_HOST_DATA_RECEIVE == 1)
    ESP_LOGI(TAG, "Host received packet - len: %d, from: " MACSTR ", to: " MACSTR, 
             len, MAC2STR(frame->src_mac), MAC2STR(frame->dst_mac));
    #endif
    
    /* paused/size/version gates, control sanitize, peers_lock + quiesce
     * re-check — shared preamble, returns with the lock held. */
    fpr_rx_ctx_t ctx;
    if (!_rx_begin(node, frame, data, len, false, &ctx)) {
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
        FPR_STORE_HASH_TYPE *existing = _peer_from_map(node, frame->src_mac);

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

            if (!_host_allow_connect(node, frame, info, existing)) {
                _peers_unlock(node);
                ESP_LOGW(TAG, "Connection from %s denied", info->name);
                return;
            }
            
            // Handle based on connection mode
            if (node->host_config.connection_mode == FPR_CONNECTION_AUTO) {
                // Auto mode - immediately accept connection
                _host_rx_auto(node, frame, info, existing);
            } else {
                // Manual mode - handle connection approval
                _host_rx_manual(node, frame, info, existing);
            }
        }
        else if (existing && existing->is_connected) {
            // Peer already connected - update timestamp and store application data
            _peer_touch(existing, frame);
            #if (FPR_DEBUG == 1)
            ESP_LOGI(TAG, "Received packet from connected peer: %s (id: %d)", existing->name, package->id);
            #endif
            _peer_store_data(node, frame, package);
        }
        else {
            // Data packet from unknown/disconnected peer - drop it
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Dropping data packet (id: %d) from unknown/disconnected peer " MACSTR,
                     package->id, MAC2STR(frame->src_mac));
            #endif
        }
    }
    _peers_unlock(node);
}

esp_err_t fpr_host_block(fpr_node_t *node, const uint8_t *peer_mac)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    FPR_STORE_HASH_TYPE *peer = _peer_lookup_locked(node, peer_mac);

    if (peer) {
        peer->is_connected = false;
        peer->state = FPR_PEER_STATE_BLOCKED;
        ESP_LOGI(TAG, "Peer blocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
        _peers_unlock(node);
    } else {
        // Add as blocked even if not in map yet
        esp_err_t err = _peer_add(node, peer_mac, "Blocked", false);
        if (err == ESP_OK) {
            peer = _peer_from_map(node, peer_mac);
            if (peer) {
                peer->state = FPR_PEER_STATE_BLOCKED;
                ESP_LOGI(TAG, "Peer blocked: " MACSTR, MAC2STR(peer_mac));
            }
        }
        _peers_unlock(node);
        return err;
    }

    return ESP_OK;
}

esp_err_t fpr_host_unblock(fpr_node_t *node, const uint8_t *peer_mac)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    FPR_STORE_HASH_TYPE *peer = _peer_lookup_locked(node, peer_mac);
    if (peer == NULL) {
        _peers_unlock(node);
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (peer->state == FPR_PEER_STATE_BLOCKED) {
        peer->state = FPR_PEER_STATE_DISCOVERED;
        ESP_LOGI(TAG, "Peer unblocked: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
        _peers_unlock(node);
        return ESP_OK;
    }
    _peers_unlock(node);

    return ESP_ERR_INVALID_STATE;
}

esp_err_t fpr_host_disconnect(fpr_node_t *node, const uint8_t *peer_mac)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    FPR_STORE_HASH_TYPE *peer = _peer_lookup_locked(node, peer_mac);
    if (peer == NULL) {
        _peers_unlock(node);
        ESP_LOGE(TAG, "Peer not found");
        return ESP_ERR_NOT_FOUND;
    }

    _reset_peer_connection(peer, FPR_PEER_STATE_DISCOVERED);
    ESP_LOGI(TAG, "Peer disconnected: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    _peers_unlock(node);

    return ESP_OK;
}

static void _host_keepalive_cb(void *key, void *value, void *user_data)
{
    (void)key;
    fpr_node_t *node = (fpr_node_t *)user_data;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    if (!peer) return;

    if (peer->state == FPR_PEER_STATE_CONNECTED) {
        int64_t age_us = esp_timer_get_time() - peer->last_seen;
        uint64_t age_ms = (uint64_t)FPR_US_TO_MS(age_us);
        if (age_ms > FPR_RECONNECT_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Client " MACSTR " timed out (age %llu ms) - disconnecting", MAC2STR(peer->mac), age_ms);
            _reset_peer_connection(peer, FPR_PEER_STATE_DISCOVERED);
            return;
        }

        // Send keepalive (device info) to client to prompt a response
        esp_err_t err = fpr_send_device_info(node, peer->mac);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Host keepalive send to " MACSTR " failed: %s", MAC2STR(peer->mac), esp_err_to_name(err));
        }
    }
}

/* One reconnect iteration: keepalive sweep + client timeouts when the
 * keepalive interval has elapsed on the caller's clock. */
void _host_reconnect_step(fpr_node_t *node, uint32_t now_ms)
{
    const uint32_t keep_interval_ms = _power_interval(node, FPR_KEEPALIVE_INTERVAL_MS);
    if ((int32_t)(now_ms - node->recon_keep_ms) >= (int32_t)keep_interval_ms) {
        _peers_lock(node);
        hashmap_foreach(&node->peers_map, _host_keepalive_cb, node);
        _peers_unlock(node);
        node->recon_keep_ms = now_ms;
    }
}



esp_err_t fpr_host_set_config(fpr_node_t *node, const fpr_host_config_t *config)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");

    /* Whole-struct copy under the lock — rx callbacks read these fields
     * (connection_mode, request_cb) while holding it. */
    _peers_lock(node);
    node->host_config = *config;
    _peers_unlock(node);
    ESP_LOGI(TAG, "Host config updated: max_peers=%d, mode=%s",
             config->max_peers,
             config->connection_mode == FPR_CONNECTION_AUTO ? "AUTO" : "MANUAL");
    return ESP_OK;
}

esp_err_t fpr_host_get_config(fpr_node_t *node, fpr_host_config_t *config)
{
    FPR_REQUIRE_NODE(node);
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");

    /* Whole-struct copy under the lock — the setter writes under it,
     * and rx callbacks read the same fields while holding it. */
    _peers_lock(node);
    *config = node->host_config;
    _peers_unlock(node);
    return ESP_OK;
}