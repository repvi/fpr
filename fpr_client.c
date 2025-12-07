#include "fpr/fpr_client.h"
#include "fpr/fpr_security.h"
#include "fpr/fpr_security_handshake.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"

typedef struct {
    uint8_t mac[MAC_ADDRESS_LENGTH];
    char name[FPR_CONNECT_NAME_SIZE];
    size_t name_size; // caller buffer size (may be 0)
    bool found;
} host_search_ctx_t;

typedef struct {
    fpr_peer_info_t *array;
    size_t max_peers;
    size_t count;
    bool connected_only;
} peer_filter_ctx_t;

static const char *TAG = "fpr_client";

extern esp_err_t fpr_network_send_device_info(uint8_t *peer_address);
extern fpr_connect_t make_fpr_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk);
extern esp_err_t fpr_network_send_to_peer(uint8_t *peer_address, void *data, int size, fpr_package_id_t package_id);

static void _check_connected_callback(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    bool *is_connected = (bool *)user_data;
    if (peer && peer->is_connected) {
        *is_connected = true;
    }
}

bool fpr_client_is_connected()
{
    bool connected = false;
    size_t count = hashmap_size(&fpr_net.peers_map);
    if (count > 0) {
        hashmap_foreach(&fpr_net.peers_map, _check_connected_callback, &connected);
    }
    return connected;
}

static void _add_and_ping_host_from_client(const esp_now_recv_info_t *esp_now_info, const fpr_connect_t *info) 
{    
    // Invoke discovery callback if registered
    if (fpr_net.client_config.discovery_cb) {
        fpr_net.client_config.discovery_cb(esp_now_info->src_addr, info->name, esp_now_info->rx_ctrl->rssi);
    }

    // Check if we're already connected to a DIFFERENT host
    // Allow reconnection to the SAME host (e.g., after restart)
    FPR_STORE_HASH_TYPE *existing = _get_peer_from_map(esp_now_info->src_addr);
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
                    _add_discovered_peer(info->name, esp_now_info->src_addr, 0, false);
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
                _add_discovered_peer(info->name, esp_now_info->src_addr, 0, false);
            }
            return;
        }
    }

    // Add as discovered FIRST (this registers the peer with ESP-NOW) - skip if already exists
    esp_err_t err = ESP_OK;
    if (!existing) {
        err = _add_discovered_peer(info->name, esp_now_info->src_addr, 0, false);
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

void _handle_client_discovery(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
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
    
    fpr_package_t *package = (fpr_package_t *)data;
    
    // Version handling (using fpr_handle.h)
    if (!fpr_version_handle_version(esp_now_info, data, len, package->version)) {
        return; // Version handler rejected the packet
    }
    
    // Connection/handshake packets use FPR_PACKET_ID_CONTROL (-1)
    // Application data packets use other IDs (0 or positive)
    bool is_control_packet = (package->id == FPR_PACKET_ID_CONTROL);
    
    fpr_connect_t *info = &package->protocol.connect_info;
    bool is_broadcast = is_address_broadcast(esp_now_info->des_addr);
    
    // Handle broadcast discovery messages from host (always control packets)
    if (is_broadcast && is_control_packet) {
        // Check if we already know this host
        FPR_STORE_HASH_TYPE *known_host = _get_peer_from_map(esp_now_info->src_addr);
        if (known_host == NULL) {
            #if (FPR_DEBUG == 1)
            ESP_LOGI(TAG, "Found new host: %s (" MACSTR ")", info->name, MAC2STR(esp_now_info->src_addr));
            #endif
            _add_and_ping_host_from_client(esp_now_info, info);
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
                    // Reset security state and reconnect (state reset is always safe)
                    known_host->sec_state = FPR_SEC_STATE_NONE;
                    known_host->is_connected = false;
                    known_host->state = FPR_PEER_STATE_DISCOVERED;
                    known_host->security.pwk_valid = false;
                    known_host->security.lwk_valid = false;
                    _update_peer_rssi_and_timestamp(known_host, esp_now_info);

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
                            bool should_connect = fpr_net.client_config.selection_cb(esp_now_info->src_addr, info->name, esp_now_info->rx_ctrl->rssi);
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
        FPR_STORE_HASH_TYPE *existing = _get_peer_from_map(esp_now_info->src_addr);
        if (existing) {
            // Update timestamp first for any unicast from known peer
            _update_peer_rssi_and_timestamp(existing, esp_now_info);
            
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
                        existing->sec_state = FPR_SEC_STATE_NONE;
                        existing->is_connected = false;
                        existing->state = FPR_PEER_STATE_DISCOVERED;
                        existing->security.pwk_valid = false;
                        existing->security.lwk_valid = false;
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
                _store_data_from_peer_helper(esp_now_info, package);
            }
        }
    }
}

void _find_host_callback(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    host_search_ctx_t *ctx = (host_search_ctx_t *)user_data;
    // Find host that is either fully connected OR in the process of connecting
    // (state >= DISCOVERED means we know about this host)
    if (peer && peer->state >= FPR_PEER_STATE_DISCOVERED && !ctx->found) {
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
    }
}

esp_err_t fpr_client_get_host_info(uint8_t *mac_out, char *name_out, size_t name_size)
{
    ESP_RETURN_ON_FALSE(mac_out != NULL, ESP_ERR_INVALID_ARG, TAG, "MAC output buffer is NULL");

    host_search_ctx_t ctx = {
        .name_size = (name_size > 0 && name_size < FPR_CONNECT_NAME_SIZE) ? name_size : FPR_CONNECT_NAME_SIZE,
        .found = false
    };

    hashmap_foreach(&fpr_net.peers_map, _find_host_callback, &ctx);

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
    
    fpr_net.client_config = *config;
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

static void _filter_peers_callback(void *key, void *value, void *user_data)
{
    (void)key;
    FPR_STORE_HASH_TYPE *peer = (FPR_STORE_HASH_TYPE *)value;
    peer_filter_ctx_t *ctx = (peer_filter_ctx_t *)user_data;
    
    if (ctx->count < ctx->max_peers && peer) {
        // Filter based on criteria
        if (ctx->connected_only && peer->state != FPR_PEER_STATE_CONNECTED) {
            return;  // Skip non-connected peers
        }
        
        _copy_peer_to_info(peer, &ctx->array[ctx->count]);
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
    
    hashmap_foreach(&fpr_net.peers_map, _filter_peers_callback, &ctx);
    return ctx.count;
}

esp_err_t fpr_client_connect_to_host(uint8_t *peer_mac, TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer MAC is NULL");
    
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_mac);
    ESP_RETURN_ON_FALSE(peer != NULL, ESP_ERR_NOT_FOUND, TAG, "Host not found - scan first");
    
    if (peer->state == FPR_PEER_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Already connected to %s", peer->name);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Connecting to host: %s (" MACSTR ")", peer->name, MAC2STR(peer_mac));
    
    // Send direct connection request to this specific host
    TickType_t start = xTaskGetTickCount();
    const TickType_t retry_interval = pdMS_TO_TICKS(FPR_MANUAL_CONNECT_RETRY_INTERVAL_MS);
    
    while (xTaskGetTickCount() - start < timeout) {
        esp_err_t err = fpr_network_send_device_info(peer_mac);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send connection request: %s", esp_err_to_name(err));
        }
        
        // Check if connected
        peer = _get_peer_from_map(peer_mac);
        if (peer && peer->state == FPR_PEER_STATE_CONNECTED) {
            ESP_LOGI(TAG, "Successfully connected to %s", peer->name);
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
        FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(host_mac);
        if (peer) {
            peer->is_connected = false;
            peer->state = FPR_PEER_STATE_DISCOVERED;
            ESP_LOGI(TAG, "Disconnected from host: %s", peer->name);
            return ESP_OK;
        }
    }
    
    return ESP_ERR_NOT_FOUND;
}

size_t fpr_client_scan_for_hosts(TickType_t duration)
{
    ESP_LOGI(TAG, "Scanning for hosts for %lu ms", (unsigned long)pdTICKS_TO_MS(duration));
    
    size_t initial_count = hashmap_size(&fpr_net.peers_map);
    TickType_t start = xTaskGetTickCount();
    const TickType_t broadcast_interval = pdMS_TO_TICKS(FPR_HOST_SCAN_BROADCAST_INTERVAL_MS);
    TickType_t last_broadcast = 0;
    
    while (xTaskGetTickCount() - start < duration) {
        TickType_t now = xTaskGetTickCount();
        if (now - last_broadcast >= broadcast_interval) {
            extern esp_err_t fpr_network_broadcast_device_info();
            fpr_network_broadcast_device_info();
            last_broadcast = now;
        }
        vTaskDelay(pdMS_TO_TICKS(FPR_HOST_SCAN_POLL_INTERVAL_MS));
    }
    
    size_t final_count = hashmap_size(&fpr_net.peers_map);
    size_t discovered = final_count > initial_count ? final_count - initial_count : 0;
    
    ESP_LOGI(TAG, "Scan complete - discovered %zu new hosts", discovered);
    return discovered;
}

void _fpr_client_reconnect_task(void *arg)
{
    (void)arg;
    TickType_t last_keep = xTaskGetTickCount();

    while (1) {
        // Get power-adjusted intervals
        const TickType_t keep_interval_ticks = pdMS_TO_TICKS(_fpr_get_power_adjusted_interval(FPR_KEEPALIVE_INTERVAL_MS));
        const TickType_t check_interval_ticks = pdMS_TO_TICKS(_fpr_get_power_adjusted_interval(FPR_CLIENT_WAIT_CHECK_INTERVAL_MS));
        
        // If connected, send periodic keepalive and check for host timeout
        uint8_t host_mac[MAC_ADDRESS_LENGTH];
        if (fpr_client_get_host_info(host_mac, NULL, 0) == ESP_OK) {
            FPR_STORE_HASH_TYPE *host_peer = _get_peer_from_map(host_mac);
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
                uint32_t timeout_ms = _fpr_get_power_adjusted_interval(FPR_RECONNECT_TIMEOUT_MS);
                if ((uint64_t)US_TO_MS(age_us) > timeout_ms) {
                    ESP_LOGW(TAG, "Host timed out (age %llu ms) - marking disconnected for reconnect", (unsigned long long)US_TO_MS(age_us));
                    host_peer->is_connected = false;
                    host_peer->state = FPR_PEER_STATE_DISCOVERED;
                }
            }
        }

        vTaskDelay(check_interval_ticks);
    }
}