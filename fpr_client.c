/**
 * @file fpr_client.c
 * @brief FPR Client Mode Implementation
 * 
 * Implements client mode functionality including host discovery,
 * connection management, and the client side of the security handshake.
 * 
 * @version 1.0.0 (Stable)
 * @date December 2025S
 */

#include "fpr/fpr.h"
#include "fpr/internal/rx.h"
#include "fpr/fpr_security.h"
#include "fpr/internal/fpr_security_handshake.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"

typedef struct {
    uint8_t mac[MAC_ADDRESS_LENGTH];
    char name[FPR_CONNECT_NAME_SIZE];
    size_t name_size; // caller buffer size (may be 0)
    bool found;
    bool found_connected;
} host_search_ctx_t;

typedef struct {
    fpr_peer_info_t *array;
    size_t max_peers;
    size_t count;
    bool connected_only;
} peer_filter_ctx_t;

static const char *TAG = "fpr_client";

static void _peer_connected_cb(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    bool *is_connected = (bool *)user_data;
    if (peer && peer->is_connected) {
        *is_connected = true;
    }
}

bool fpr_client_is_connected(void)
{
    _peers_lock();
    bool connected = false;
    size_t count = hashmap_size(&fpr_net.peers_map);
    if (count > 0) {
        hashmap_foreach(&fpr_net.peers_map, _peer_connected_cb, &connected);
    }
    _peers_unlock();
    return connected;
}

static void _client_discover_host(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info) 
{    
    // Invoke discovery callback if registered
    if (fpr_net.client_config.discovery_cb) {
        fpr_net.client_config.discovery_cb(esp_now_info->src_addr, info->name, esp_now_info->rx_ctrl->rssi);
    }

    // Check if we're already connected to a DIFFERENT host
    // Allow reconnection to the SAME host (e.g., after restart)
    FPR_STORE_HASH_TYPE *existing = _peer_from_map(esp_now_info->src_addr);
    if (!existing && fpr_client_is_connected()) {
        #if (FPR_DEBUG == 1)
        ESP_LOGW(TAG, "Already connected to a different host - ignoring %s", info->name);
        #endif
        return;
    }

    // In manual mode, consult selection callback if provided. If selection_cb is NULL,
    // treat it as an explicit "decline" and do not auto-connect.
    if (fpr_net.client_config.connection_mode == FPR_CONNECTION_MANUAL) {
        if (fpr_net.client_config.selection_cb != NULL) {
            bool should_connect = fpr_net.client_config.selection_cb(esp_now_info->src_addr, info->name, esp_now_info->rx_ctrl->rssi);
            if (!should_connect) {
                #if (FPR_DEBUG == 1)
                ESP_LOGI(TAG, "Application declined connection to host: %s", info->name);
                #endif
                // Still add as discovered but don't initiate connection
                if (!existing) {
                    _peer_add_discovered(info->name, esp_now_info->src_addr, 0, false);
                }
                return;
            }
            ESP_LOGI(TAG, "Application approved connection to host: %s", info->name);
        } else {
            #if (FPR_DEBUG == 1)
            ESP_LOGI(TAG, "Manual mode and no selection callback provided - not auto-connecting to host: %s", info->name);
            #endif
            // Add as discovered but do not initiate handshake
            if (!existing) {
                _peer_add_discovered(info->name, esp_now_info->src_addr, 0, false);
            }
            return;
        }
    }

    // Add as discovered FIRST (this registers the peer with ESP-NOW) - skip if already exists
    esp_err_t err = ESP_OK;
    if (!existing) {
        err = _peer_add_discovered(info->name, esp_now_info->src_addr, 0, false);
    }
    
    if (err == ESP_OK) {
        // Send initial discovery request without keys
        err = fpr_network_send_device_info(esp_now_info->src_addr);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sent initial discovery to host " MACSTR, MAC2STR(esp_now_info->src_addr));
        } else {
            ESP_LOGE(TAG, "Failed to send discovery to host: %s", esp_err_to_name(err));
        }
        
        if (fpr_net.client_config.connection_mode == FPR_CONNECTION_AUTO) {
            ESP_LOGI(TAG, "Host discovered: %s (waiting for PWK)", info->name);
        } else {
            ESP_LOGI(TAG, "Host discovered: %s (manual connection approved)", info->name);
        }
    } else {
        ESP_LOGE(TAG, "Failed to add discovered host: %s", esp_err_to_name(err));
    }
}

void _rx_client(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    #if (FPR_DEBUG_LOG_CLIENT_DATA_RECEIVE == 1)
    ESP_LOGI(TAG, "Client received packet - len: %d, from: " MACSTR ", to: " MACSTR,
             len, MAC2STR(esp_now_info->src_addr), MAC2STR(esp_now_info->des_addr));
    #endif
    
    // Check if network is paused
    if (fpr_net.paused) {
        return;  // Drop all packets when paused
    }
    
    if (!is_fpr_package_compatible(len)) {
        return;
    }
    
    const fpr_package_t *package = (const fpr_package_t *)data;
    
    // Version handling (using fpr_handle.h)
    if (!fpr_version_handle_version(esp_now_info, data, len, package->version)) {
        return; // Version handler rejected the packet
    }
    
    // Connection/handshake packets use FPR_PACKET_ID_CONTROL (-1)
    // Application data packets use other IDs (0 or positive)
    bool is_control_packet = (package->id == FPR_PACKET_ID_CONTROL);
    
    /* Sanitize a local control message, never the driver-owned packet:
     * the same union holds application bytes for non-control packets. */
    fpr_connect_t control_info = {0};
    if (is_control_packet) {
        memcpy(&control_info, &package->protocol.connect_info, sizeof(control_info));
        control_info.name[FPR_CONNECT_NAME_SIZE - 1] = '\0';
    }
    const fpr_connect_t *info = &control_info;
    bool is_broadcast = is_broadcast_address(esp_now_info->des_addr);
    
    // Handle broadcast discovery messages from host (always control packets)
    /* Serialize the whole peer-map region (get/add/handshake/data store)
     * against remove/clear on other tasks. Recursive: discovery_cb and
     * data_callback below may re-enter peer APIs on this same task. */
    _peers_lock();
    /* Re-check the quiesce flag inside the lock: deinit flips state to
     * UNINITIALIZED before draining — a callback dispatched just before
     * unregister may only reach the lock now and must not touch maps. */
    if (fpr_net.state == FPR_STATE_UNINITIALIZED) {
        _peers_unlock();
        return;
    }
    if (is_broadcast && is_control_packet) {
        // Check if we already know this host
        FPR_STORE_HASH_TYPE *known_host = _peer_from_map(esp_now_info->src_addr);
        if (known_host == NULL) {
            #if (FPR_DEBUG == 1)
            ESP_LOGI(TAG, "Found new host: %s (" MACSTR ")", info->name, MAC2STR(esp_now_info->src_addr));
            #endif
            _client_discover_host(esp_now_info, info);
        } else {
            // Known host - check if we need to reconnect (e.g., after host restart)
            // If host restarted, it lost our connection info and is broadcasting again
            // We should reinitiate connection if not fully connected
            // BUT: Don't interrupt an in-progress handshake (sec_state > NONE)
            if (!known_host->is_connected || known_host->sec_state != FPR_SEC_STATE_ESTABLISHED) {
                // Only reset if we're not already in the middle of a handshake
                if (known_host->sec_state == FPR_SEC_STATE_NONE) {
                    ESP_LOGI(TAG, "Host %s broadcast received - reinitiating connection (current state=%d, connected=%d)",
                             info->name, known_host->sec_state, known_host->is_connected);
                    // Reset connection/session state and reconnect
                    _reset_peer_connection(known_host, FPR_PEER_STATE_DISCOVERED);
                    _peer_touch(known_host, esp_now_info);

                    // Decide whether to initiate handshake automatically based on mode
                    if (fpr_net.client_config.connection_mode == FPR_CONNECTION_AUTO) {
                        esp_err_t err = fpr_network_send_device_info(esp_now_info->src_addr);
                        if (err == ESP_OK) {
                            ESP_LOGI(TAG, "Sent reconnection request to host " MACSTR, MAC2STR(esp_now_info->src_addr));
                        } else {
                            ESP_LOGE(TAG, "Failed to send reconnection request: %s", esp_err_to_name(err));
                        }
                    } else {
                        // Manual mode: consult selection_cb if present; if NULL, do not auto-reconnect
                        if (fpr_net.client_config.selection_cb) {
                            int8_t rssi = esp_now_info->rx_ctrl ? esp_now_info->rx_ctrl->rssi : 0;
                            bool should_connect = fpr_net.client_config.selection_cb(esp_now_info->src_addr, info->name, rssi);
                            if (should_connect) {
                                esp_err_t err = fpr_network_send_device_info(esp_now_info->src_addr);
                                if (err == ESP_OK) {
                                    ESP_LOGI(TAG, "Sent reconnection request to host " MACSTR, MAC2STR(esp_now_info->src_addr));
                                } else {
                                    ESP_LOGE(TAG, "Failed to send reconnection request: %s", esp_err_to_name(err));
                                }
                            } else {
                                #if (FPR_DEBUG == 1)
                                ESP_LOGI(TAG, "Manual mode: application declined reconnect to host: %s", info->name);
                                #endif
                            }
                        } else {
                            #if (FPR_DEBUG == 1)
                            ESP_LOGI(TAG, "Manual mode and no selection callback provided - not auto-reconnecting to host: %s", info->name);
                            #endif
                        }
                    }
                }
                #if (FPR_DEBUG == 1)
                else {
                    ESP_LOGD(TAG, "Ignoring broadcast - handshake in progress (state=%d)", known_host->sec_state);
                }
                #endif
            }
        }
    } else if (!is_broadcast) {
        // Handle unicast response from host (security handshake or data)
        FPR_STORE_HASH_TYPE *existing = _peer_from_map(esp_now_info->src_addr);
        if (existing) {
            // Update timestamp first for any unicast from known peer
            _peer_touch(existing, esp_now_info);
            
            // Handle security handshake (WiFi-style) - only for control packets (id == -1)
            // In manual mode, host initiates handshake after approval
            if (is_control_packet) {
                if (info->has_pwk && !info->has_lwk) {
                    // Step 2: Received PWK from host
                    
                    // CRITICAL: If we receive a PWK while in ESTABLISHED or later state,
                    // it means the host has restarted and lost our connection.
                    // We must reset our state and restart the handshake.
                    if (existing->sec_state >= FPR_SEC_STATE_LWK_SENT) {
                        ESP_LOGI(TAG, "Host %s appears to have restarted (received PWK while in state %d) - resetting connection",
                                 existing->name, existing->sec_state);
                        _reset_peer_connection(existing, FPR_PEER_STATE_DISCOVERED);
                    }
                    
                    // Only process if we haven't already received and processed a PWK
                    if (existing->sec_state < FPR_SEC_STATE_PWK_RECEIVED) {
                        // Generate own LWK and send PWK+LWK back
                        fpr_sec_client_handle_pwk(esp_now_info->src_addr, existing, info);
                    }
                    #if (FPR_DEBUG == 1)
                    else {
                        ESP_LOGW(TAG, "Ignoring duplicate PWK - already in handshake (current_state=%d, expected<%d)", 
                                 existing->sec_state, FPR_SEC_STATE_PWK_RECEIVED);
                    }
                    #endif
                } else if (info->has_pwk && info->has_lwk) {
                    // Step 4: Received acknowledgment from host with PWK+LWK
                    
                    // CRITICAL: If we're already ESTABLISHED and receive an ACK,
                    // it's likely a retransmitted packet - safe to re-verify or ignore.
                    // If we're in NONE/DISCOVERED, this is unexpected but could mean
                    // we missed the PWK - let's process it anyway.
                    if (existing->sec_state == FPR_SEC_STATE_ESTABLISHED) {
                        #if (FPR_DEBUG == 1)
                        ESP_LOGD(TAG, "Received ACK while already established - likely retransmit, ignoring");
                        #endif
                        _peers_unlock();
                        return; // Already connected, ignore duplicate ACK
                    }
                    
                    // Process ACK if we're in the correct state (LWK_SENT) or earlier
                    if (existing->sec_state == FPR_SEC_STATE_LWK_SENT) {
                        // Verify and mark connected
                        fpr_sec_client_verify_ack(esp_now_info->src_addr, existing, info);
                    }
                    #if (FPR_DEBUG == 1)
                    else {
                        ESP_LOGW(TAG, "Ignoring ACK - wrong state (current=%d, expected=%d, has_pwk=%d, has_lwk=%d)", 
                                 existing->sec_state, FPR_SEC_STATE_LWK_SENT, info->has_pwk, info->has_lwk);
                    }
                    #endif
                }
            }
            
            // If already connected, store application data (non-control packets)
            if (existing->is_connected && !is_control_packet) {
                // Store data from connected peers (unicast only)
                #if (FPR_DEBUG == 1)
                ESP_LOGI(TAG, "Received data from connected host: %s (id: %d)", existing->name, package->id);
                #endif
                _peer_store_data(esp_now_info, package);
            }
        }
    }
    _peers_unlock();
}

void _host_find_cb(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    host_search_ctx_t *ctx = (host_search_ctx_t *)user_data;
    // Hosts we can use are DISCOVERED/PENDING/CONNECTED — REJECTED and
    // BLOCKED are deliberately excluded. Prefer a CONNECTED host over a
    // merely-known one when several peers exist.
    const bool usable = peer && peer->state <= FPR_PEER_STATE_CONNECTED;
    const bool upgrade = ctx->found && !ctx->found_connected &&
                         peer && peer->state == FPR_PEER_STATE_CONNECTED;
    if (usable && (!ctx->found || upgrade)) {
        memcpy(ctx->mac, peer->peer_info.peer_addr, MAC_ADDRESS_LENGTH);
        if (ctx->name_size > 0) {
            // copy up to caller-provided size (reserve space for NUL)
            size_t copy_len = ctx->name_size - 1;
            if (copy_len > 0) {
                strncpy(ctx->name, peer->name, copy_len);
                ctx->name[copy_len] = '\0';
            } else {
                ctx->name[0] = '\0';
            }
        } else {
            // ensure internal name buffer is NUL-terminated
            ctx->name[0] = '\0';
        }
        ctx->found = true;
        ctx->found_connected = (peer->state == FPR_PEER_STATE_CONNECTED);
    }
}

esp_err_t fpr_client_get_host_info(uint8_t *mac_out, char *name_out, size_t name_size)
{
    ESP_RETURN_ON_FALSE(mac_out != NULL, ESP_ERR_INVALID_ARG, TAG, "MAC output buffer is NULL");

    host_search_ctx_t ctx = {
        .name_size = (name_size > 0 && name_size < FPR_CONNECT_NAME_SIZE) ? name_size : FPR_CONNECT_NAME_SIZE,
        .found = false
    };

    _peers_lock();
    hashmap_foreach(&fpr_net.peers_map, _host_find_cb, &ctx);
    _peers_unlock();

    if (!ctx.found) {
        return ESP_ERR_NOT_FOUND;
    }

    // copy results back to caller buffers
    memcpy(mac_out, ctx.mac, MAC_ADDRESS_LENGTH);
    if (name_out && name_size > 0) {
        // copy up to caller buffer, ensure NUL
        size_t cpy = name_size - 1;
        strncpy(name_out, ctx.name, cpy);
        name_out[cpy] = '\0';
    }

    return ESP_OK;
}

esp_err_t fpr_client_set_config(const fpr_client_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");

    /* Whole-struct copy under the lock — rx callbacks read these fields
     * (connection_mode, callbacks) while holding it. */
    _peers_lock();
    fpr_net.client_config = *config;
    _peers_unlock();
    ESP_LOGI(TAG, "Client config updated: mode=%s",
             config->connection_mode == FPR_CONNECTION_AUTO ? "AUTO" : "MANUAL");
    return ESP_OK;
}

esp_err_t fpr_client_get_config(fpr_client_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    
    *config = fpr_net.client_config;
    return ESP_OK;
}

static void _peer_filter_cb(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    peer_filter_ctx_t *ctx = (peer_filter_ctx_t *)user_data;
    
    if (ctx->count < ctx->max_peers && peer) {
        // Filter based on criteria
        if (ctx->connected_only && peer->state != FPR_PEER_STATE_CONNECTED) {
            return;  // Skip non-connected peers
        }
        
        _peer_copy_to_info(peer, &ctx->array[ctx->count]);
        ctx->count++;
    }
}

size_t fpr_client_list_discovered_hosts(fpr_peer_info_t *peer_array, size_t max_peers)
{
    if (!peer_array || max_peers == 0) {
        return 0;
    }
    
    peer_filter_ctx_t ctx = {
        .array = peer_array,
        .max_peers = max_peers,
        .count = 0,
        .connected_only = false  // List all discovered hosts
    };

    _peers_lock();
    hashmap_foreach(&fpr_net.peers_map, _peer_filter_cb, &ctx);
    _peers_unlock();
    return ctx.count;
}

esp_err_t fpr_client_connect_to_host(const uint8_t *peer_mac, TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");

    _peers_lock();
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_mac);
    if (peer == NULL) {
        _peers_unlock();
        ESP_LOGE(TAG, "Host not found - scan first");
        return ESP_ERR_NOT_FOUND;
    }

    if (peer->state == FPR_PEER_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Already connected to %s", peer->name);
        _peers_unlock();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Connecting to host: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    _peers_unlock();

    // Send direct connection request to this specific host
    TickType_t start = xTaskGetTickCount();
    const TickType_t retry_interval = pdMS_TO_TICKS(FPR_MANUAL_CONNECT_RETRY_INTERVAL_MS);

    while (xTaskGetTickCount() - start < timeout) {
        esp_err_t err = fpr_network_send_device_info(peer_mac);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send connection request: %s", esp_err_to_name(err));
        }

        // Check if connected — re-lookup under the lock each iteration;
        // the store can be freed by a concurrent remove between rounds.
        _peers_lock();
        peer = _peer_from_map(peer_mac);
        bool connected = (peer && peer->state == FPR_PEER_STATE_CONNECTED);
        _peers_unlock();
        if (connected) {
            ESP_LOGI(TAG, "Successfully connected to host " MACSTR, MAC2STR(peer_mac));
            return ESP_OK;
        }

        vTaskDelay(retry_interval);
    }
    
    ESP_LOGW(TAG, "Connection timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t fpr_client_disconnect(void)
{
    // Find and disconnect from connected host
    uint8_t host_mac[6];
    esp_err_t err = fpr_client_get_host_info(host_mac, NULL, 0);
    if (err == ESP_OK) {
        _peers_lock();
        FPR_STORE_HASH_TYPE *peer = _peer_from_map(host_mac);
        if (peer) {
            _reset_peer_connection(peer, FPR_PEER_STATE_DISCOVERED);
            ESP_LOGI(TAG, "Disconnected from host: %s", peer->name);
            _peers_unlock();
            return ESP_OK;
        }
        _peers_unlock();
    }

    return ESP_ERR_NOT_FOUND;
}

size_t fpr_client_scan_for_hosts(TickType_t duration)
{
    ESP_LOGI(TAG, "Scanning for hosts for %lu ms", (unsigned long)pdTICKS_TO_MS(duration));

    _peers_lock();
    size_t initial_count = hashmap_size(&fpr_net.peers_map);
    _peers_unlock();
    TickType_t start = xTaskGetTickCount();
    const TickType_t broadcast_interval = pdMS_TO_TICKS(FPR_HOST_SCAN_BROADCAST_INTERVAL_MS);
    TickType_t last_broadcast = 0;
    
    while (xTaskGetTickCount() - start < duration) {
        TickType_t now = xTaskGetTickCount();
        if (now - last_broadcast >= broadcast_interval) {
            fpr_network_broadcast_device_info();
            last_broadcast = now;
        }
        vTaskDelay(pdMS_TO_TICKS(FPR_HOST_SCAN_POLL_INTERVAL_MS));
    }
    
    _peers_lock();
    size_t final_count = hashmap_size(&fpr_net.peers_map);
    _peers_unlock();
    size_t discovered = final_count > initial_count ? final_count - initial_count : 0;
    
    ESP_LOGI(TAG, "Scan complete - discovered %zu new hosts", discovered);
    return discovered;
}

void _task_client_reconnect(void *arg)
{
    (void)arg;
    TickType_t last_keep = xTaskGetTickCount();

    while (!fpr_net.reconnect_stop_requested) {
        // Get power-adjusted intervals
        const TickType_t keep_interval_ticks = pdMS_TO_TICKS(_power_interval(FPR_KEEPALIVE_INTERVAL_MS));
        const TickType_t check_interval_ticks = pdMS_TO_TICKS(_power_interval(FPR_CLIENT_WAIT_CHECK_INTERVAL_MS));
        
        // If connected, send periodic keepalive and check for host timeout
        uint8_t host_mac[MAC_ADDRESS_LENGTH];
        if (fpr_client_get_host_info(host_mac, NULL, 0) == ESP_OK) {
            _peers_lock();
            FPR_STORE_HASH_TYPE *host_peer = _peer_from_map(host_mac);
            if (host_peer && host_peer->is_connected) {
                // keepalive send periodically
                if ((xTaskGetTickCount() - last_keep) >= keep_interval_ticks) {
                    esp_err_t err = fpr_network_send_device_info(host_mac);
                    if (err != ESP_OK) {
                        ESP_LOGD(TAG, "Keepalive to host failed: %s", esp_err_to_name(err));
                    }
                    last_keep = xTaskGetTickCount();
                }

                // check last seen timestamp (in microseconds)
                int64_t age_us = esp_timer_get_time() - host_peer->last_seen;
                uint32_t timeout_ms = _power_interval(FPR_RECONNECT_TIMEOUT_MS);
                if ((uint64_t)FPR_US_TO_MS(age_us) > timeout_ms) {
                    ESP_LOGW(TAG, "Host timed out (age %llu ms) - marking disconnected for reconnect", (unsigned long long)FPR_US_TO_MS(age_us));
                    _reset_peer_connection(host_peer, FPR_PEER_STATE_DISCOVERED);
                }
            } else if (host_peer && !host_peer->is_connected &&
                       host_peer->sec_state > FPR_SEC_STATE_NONE &&
                       host_peer->sec_state < FPR_SEC_STATE_ESTABLISHED) {
                // Handshake stalled (e.g. the host's ACK was lost). Nothing
                // else times these states out; clear them so the next host
                // broadcast can restart the handshake instead of us sitting
                // in LWK_SENT forever.
                int64_t age_us = esp_timer_get_time() - host_peer->last_seen;
                uint32_t timeout_ms = _power_interval(FPR_RECONNECT_TIMEOUT_MS);
                if ((uint64_t)FPR_US_TO_MS(age_us) > timeout_ms) {
                    ESP_LOGW(TAG, "Handshake with host stalled in state %d - resetting",
                             host_peer->sec_state);
                    _reset_peer_connection(host_peer, FPR_PEER_STATE_DISCOVERED);
                }
            }
            _peers_unlock();
        }

        vTaskDelay(check_interval_ticks);
    }
    _task_exit(&fpr_net.reconnect_task);
}
