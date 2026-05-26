/**
 * @file send.c
 * @brief Sprout Network Transmit Functions
 *
 * All packet transmission functions including send with options,
 * broadcast, and device info transmission.
 */

#include "sprout/internal/helpers.h"
#include "sprout/sprout.h"
#include "sprout/internal/private_defs.h"
#include "sprout/sprout_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"

static const char *TAG = "sprout_send";

extern sprout_network_t sprout_net;

static esp_err_t sprout_send_helper(uint8_t *peer_address, void *data, int size, sprout_package_id_t package_id)
{
    ESP_RETURN_ON_FALSE(data != NULL && size > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid data or size");

    if (sprout_net.paused) {
        ESP_LOGW(TAG, "Network is paused - send operation blocked");
        return ESP_ERR_INVALID_STATE;
    }

    const size_t PROTOCOL_SIZE = sizeof(((sprout_package_t *)0)->protocol);
    int data_remaining = size;
    bool single_packet = ((size_t)size <= PROTOCOL_SIZE);
    bool is_first_packet = true;
    uint8_t *data_ptr = (uint8_t *)data;
    esp_err_t last_result = ESP_OK;

    uint32_t seq_num = ++sprout_net.tx_sequence_num;

    while (data_remaining > 0) {
        sprout_package_t package = {0};
        size_t chunk_size = ((size_t)data_remaining <= PROTOCOL_SIZE) ? (size_t)data_remaining : PROTOCOL_SIZE;
        bool is_last_packet = ((size_t)data_remaining <= PROTOCOL_SIZE);

        if (single_packet) {
            package.package_type = SPROUT_PACKAGE_TYPE_SINGLE;
        } else if (is_first_packet) {
            package.package_type = SPROUT_PACKAGE_TYPE_START;
        } else if (is_last_packet) {
            package.package_type = SPROUT_PACKAGE_TYPE_END;
        } else {
            package.package_type = SPROUT_PACKAGE_TYPE_CONTINUED;
        }

        package.id = package_id;
        package.payload_size = (uint16_t)chunk_size;
        package.sequence_num = seq_num;
        memcpy(&package.protocol, data_ptr, chunk_size);

        memcpy(package.origin_mac, sprout_net.mac, 6);
        if (peer_address) {
            memcpy(package.dest_mac, peer_address, 6);
        } else {
            memset(package.dest_mac, 0xFF, 6);
        }
        package.hop_count = 0;
        package.max_hops = SPROUT_DEFAULT_MAX_HOPS;
        package.version = SPROUT_NETWORK_VERSION;

        last_result = esp_now_send(peer_address, (const uint8_t *)&package, sizeof(package));
        if (last_result == ESP_OK) {
            sprout_net.stats.packets_sent++;
        } else {
            sprout_net.stats.send_failures++;
            if (last_result == ESP_ERR_ESPNOW_NO_MEM) {
                ESP_LOGW(TAG, "ESP-NOW buffer full (NO_MEM), try reducing send rate or increasing receive processing");
            }
            #if (SPROUT_DEBUG == 1)
            ESP_LOGW(TAG, "esp_now_send failed: %s (0x%x)", esp_err_to_name(last_result), last_result);
            #endif
            return last_result;
        }

        data_ptr += chunk_size;
        data_remaining -= (int)chunk_size;
        is_first_packet = false;

        if (!single_packet && data_remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return last_result;
}

esp_err_t sprout_send(uint8_t *peer_address, void *data, int size, sprout_package_id_t package_id)
{
    if (package_id != SPROUT_PACKET_ID_CONTROL) {
        SPROUT_STORE_HASH_TYPE *peer = _get_peer_from_map(peer_address);
        if (peer == NULL) {
            #if (SPROUT_DEBUG == 1)
            ESP_LOGW(TAG, "Attempting to send to unknown peer " MACSTR, MAC2STR(peer_address));
            #endif
            return ESP_ERR_NOT_FOUND;
        }
        if (!peer->is_connected) {
            #if (SPROUT_DEBUG == 1)
            ESP_LOGW(TAG, "Attempting to send to disconnected peer %s (" MACSTR ")", peer->name, MAC2STR(peer_address));
            #endif
            return ESP_ERR_INVALID_STATE;
        }
    }

    return sprout_send_helper(peer_address, data, size, package_id);
}

esp_err_t sprout_broadcast(void *data, int size, sprout_package_id_t package_id)
{
    uint8_t broadcast_mac[6] = SPROUT_BROADCAST_ADDRESS;
    return sprout_send_helper(broadcast_mac, data, size, package_id);
}

sprout_connect_t make_sprout_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk)
{
    sprout_connect_t info = {0};
    _safe_string_copy(info.name, sprout_net.name, sizeof(info.name));
    memcpy(info.peer_info.peer_addr, sprout_net.mac, 6);
    sprout_set_peer_info(&info.peer_info);
    info.visibility = sprout_net.access_state;

    if (include_pwk && pwk) {
        memcpy(info.pwk, pwk, SPROUT_KEY_SIZE);
        info.has_pwk = true;
    } else {
        info.has_pwk = false;
    }

    if (include_lwk && lwk) {
        memcpy(info.lwk, lwk, SPROUT_KEY_SIZE);
        info.has_lwk = true;
    } else {
        info.has_lwk = false;
    }

    return info;
}

static sprout_connect_t make_sprout_info()
{
    return make_sprout_info_with_keys(false, false, NULL, NULL);
}

esp_err_t sprout_send_device_info(uint8_t *peer_address)
{
    sprout_connect_t info = make_sprout_info();
    return sprout_send(peer_address, (void *)&info, sizeof(info), SPROUT_PACKET_ID_CONTROL);
}

esp_err_t sprout_broadcast_device_info()
{
    sprout_connect_t info = make_sprout_info();
    return sprout_broadcast((void *)&info, sizeof(info), SPROUT_PACKET_ID_CONTROL);
}
