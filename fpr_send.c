/**
 * @file fpr_send.c
 * @brief FPR Send Path: fragmentation, unicast/broadcast sends, device-info packets
 */
#include "fpr/fpr.h"
#include "fpr/internal/helpers.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "fpr_send";
esp_err_t fpr_send_with_options(const uint8_t *peer_address, const void *data, int size, const fpr_send_options_t *options)
{
    ESP_RETURN_ON_FALSE(data != NULL && size > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid data or size");
    ESP_RETURN_ON_FALSE(options != NULL, ESP_ERR_INVALID_ARG, TAG, "Options cannot be NULL");
    
    // Check if network is paused
    if (fpr_net.paused) {
        ESP_LOGW(TAG, "Network is paused - send operation blocked");
        return ESP_ERR_INVALID_STATE;
    }
    
    const size_t PROTOCOL_SIZE = sizeof(((fpr_package_t *)0)->protocol);
    int data_remaining = size;
    bool single_packet = ((size_t)size <= PROTOCOL_SIZE);
    bool is_first_packet = true;
    const uint8_t *data_ptr = data;
    esp_err_t last_result = ESP_OK;
    
    /* Sequence numbers are shared across senders — the app task, the
     * rx path's handshake replies, and reconnect tasks all bump this.
     * Take the (recursive) lock so cross-context sends can't mint
     * duplicates that a receiver's replay check would drop. */
    _peers_lock();
    uint32_t seq_num = ++fpr_net.tx_sequence_num;
    _peers_unlock();
    
    while (data_remaining > 0) {
        fpr_package_t package = {0};
        size_t chunk_size = ((size_t)data_remaining <= PROTOCOL_SIZE) ? (size_t)data_remaining : PROTOCOL_SIZE;
        bool is_last_packet = ((size_t)data_remaining <= PROTOCOL_SIZE);
        
        // Determine packet type
        if (single_packet) {
            package.package_type = FPR_PACKAGE_TYPE_SINGLE;
        } else if (is_first_packet) {
            package.package_type = FPR_PACKAGE_TYPE_START;
        } else if (is_last_packet) {
            package.package_type = FPR_PACKAGE_TYPE_END;
        } else {
            package.package_type = FPR_PACKAGE_TYPE_CONTINUED;
        }
        
        package.id = options->package_id;
        package.payload_size = (uint16_t)chunk_size;  // Track actual bytes in this packet
        package.sequence_num = seq_num;               // Sequence number for replay protection
        memcpy(&package.protocol, data_ptr, chunk_size);
        
        // Initialize routing fields
        memcpy(package.origin_mac, fpr_net.mac, 6);
        if (peer_address) {
            memcpy(package.dest_mac, peer_address, 6);
        } else {
            memset(package.dest_mac, 0xFF, 6);
        }
        package.hop_count = 0;
        package.max_hops = options->max_hops > 0 ? options->max_hops : FPR_DEFAULT_MAX_HOPS;
        package.version = FPR_PROTOCOL_VERSION;  // Set protocol version
        
        last_result = esp_now_send(peer_address, (const uint8_t *)&package, sizeof(package));
        _peers_lock();
        if (last_result == ESP_OK) {
            fpr_net.stats.packets_sent++;
        } else {
            fpr_net.stats.send_failures++;
        }
        _peers_unlock();
        if (last_result != ESP_OK) {
            // Log specific error for debugging
            if (last_result == ESP_ERR_ESPNOW_NO_MEM) {
                ESP_LOGW(TAG, "ESP-NOW buffer full (NO_MEM), try reducing send rate or increasing receive processing");
            }
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "esp_now_send failed: %s (0x%x)", esp_err_to_name(last_result), last_result);
            #endif
            return last_result; // Fail early on send error
        }

        data_ptr += chunk_size;
        data_remaining -= (int)chunk_size;
        is_first_packet = false;
        
        // Small delay between fragments to prevent overwhelming the receiver
        // Only needed for multi-packet transfers
        if (!single_packet && data_remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return last_result;
}

// Thin wrapper: fpr_send_with_options() fragments payloads larger than
// the per-packet protocol window automatically.
static esp_err_t fpr_network_send_helper(const uint8_t *peer_address, const void *data, int size, fpr_package_id_t package_id)
{
    fpr_send_options_t options = {
        .package_id = package_id,
        .max_hops = FPR_DEFAULT_MAX_HOPS
    };
    return fpr_send_with_options(peer_address, data, size, &options);
}

esp_err_t fpr_network_send_to_peer(const uint8_t *peer_address, const void *data, int size, fpr_package_id_t package_id)
{
    ESP_RETURN_ON_FALSE(peer_address != NULL, ESP_ERR_INVALID_ARG, TAG, "Peer address is NULL");

    // Validate peer exists and is connected (unless it's a control packet for handshake)
    if (package_id != FPR_PACKET_ID_CONTROL) {
        _peers_lock();
        FPR_STORE_HASH_TYPE *peer = _peer_from_map(peer_address);
        if (peer == NULL) {
            _peers_unlock();
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Attempting to send to unknown peer " MACSTR, MAC2STR(peer_address));
            #endif
            return ESP_ERR_NOT_FOUND;
        }
        if (!peer->is_connected) {
            _peers_unlock();
            #if (FPR_DEBUG == 1)
            ESP_LOGW(TAG, "Attempting to send to disconnected peer %s (" MACSTR ")", peer->name, MAC2STR(peer_address));
            #endif
            return ESP_ERR_INVALID_STATE;
        }
        _peers_unlock();
    }

    return fpr_network_send_helper(peer_address, data, size, package_id);
}

esp_err_t fpr_network_broadcast(const void *data, int size, fpr_package_id_t package_id)
{
    uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    return fpr_network_send_helper(broadcast_mac, data, size, package_id);
}

// Create connection info with optional security keys
// used externally
fpr_connect_t make_fpr_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk) 
{
    fpr_connect_t info = {0}; // Zero-initialize
    _safe_string_copy(info.name, fpr_net.name, sizeof(info.name));
    memcpy(info.peer_info.peer_addr, fpr_net.mac, 6);
    fpr_set_peer_info(&info.peer_info); // Initialize peer_info properly
    info.visibility = fpr_net.access_state;
    
    // Include PWK if requested and available
    if (include_pwk && pwk) {
        memcpy(info.pwk, pwk, FPR_KEY_SIZE);
        info.has_pwk = true;
    } else {
        info.has_pwk = false;
    }
    
    // Include LWK if requested and available
    if (include_lwk && lwk) {
        memcpy(info.lwk, lwk, FPR_KEY_SIZE);
        info.has_lwk = true;
    } else {
        info.has_lwk = false;
    }
    
    return info;
}

static fpr_connect_t make_fpr_info(void)
{
    // No keys by default
    return make_fpr_info_with_keys(false, false, NULL, NULL);
}

esp_err_t fpr_network_send_device_info(const uint8_t *peer_address)
{
    fpr_connect_t info = make_fpr_info();
    return fpr_network_send_to_peer(peer_address, (void *)&info, sizeof(info), FPR_PACKET_ID_CONTROL);
}

esp_err_t fpr_network_broadcast_device_info(void)
{
    fpr_connect_t info = make_fpr_info();
    return fpr_network_broadcast((void *)&info, sizeof(info), FPR_PACKET_ID_CONTROL);
}
