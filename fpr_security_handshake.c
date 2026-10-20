/**
 * @file fpr_security_handshake.c
 * @brief FPR Security Handshake Helper Functions
 * 
 * Implements the WiFi WPA2-style 4-way handshake for both host and
 * client modes. Handles key exchange and connection establishment.
 * 
 * @version 1.0.0
 * @date December 2025
 */

#include "fpr/fpr.h"
#include "fpr/internal/fpr_security_handshake.h"
#include "fpr/internal/helpers.h"
#include "fpr/fpr_security.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "fpr_sec_handshake";

esp_err_t fpr_sec_host_send_pwk(fpr_node_t *net, const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const uint8_t *host_pwk)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && peer != NULL && host_pwk != NULL, 
                        ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    
    ESP_LOGI(TAG, "Sending PWK to client: %s", peer->name);
    fpr_connect_t response = _make_fpr_info_with_keys(net, true, false, host_pwk, NULL);
    /* Pre-arm the state BEFORE the send: on a synchronous transport the
     * client's PWK+LWK reply is processed inside fpr_node_send() and is
     * gated on sec_state==PWK_SENT — and a post-send assignment would
     * clobber the ESTABLISHED state that nested reply set. */
    memcpy(peer->security.pwk, host_pwk, FPR_KEY_SIZE);
    peer->security.pwk_valid = true;
    peer->sec_state = FPR_SEC_STATE_PWK_SENT;
    esp_err_t err = fpr_node_send(net, peer_mac, &response, sizeof(response), FPR_PACKET_ID_CONTROL);

    if (err != ESP_OK) {
        peer->sec_state = FPR_SEC_STATE_NONE;
        peer->security.pwk_valid = false;
    }

    return err;
}

esp_err_t fpr_sec_host_verify_and_ack(fpr_node_t *net, const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info, const uint8_t *host_pwk)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && peer != NULL && info != NULL && host_pwk != NULL, 
                        ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    
    // Verify PWK from client
    if (!fpr_security_verify_pwk(info->pwk, host_pwk)) {
        ESP_LOGW(TAG, "PWK verification failed from client");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Store client's LWK (client generated this)
    ESP_LOGI(TAG, "Received client LWK from: %s", peer->name);
    memcpy(peer->security.lwk, info->lwk, FPR_KEY_SIZE);
    peer->security.lwk_valid = true;

    // Mark as connected on host side BEFORE sending the ack: on a
    // synchronous transport the client processes the ack inside
    // fpr_node_send() and may immediately send data — the peer must
    // already read CONNECTED. Revert everything if the ack send fails.
    const fpr_peer_state_t prev_state = peer->state;
    peer->is_connected = true;
    peer->state = FPR_PEER_STATE_CONNECTED;
    peer->sec_state = FPR_SEC_STATE_ESTABLISHED;

    // Fresh session: reset sequence tracking and drain stale packets
    _peer_clear_session(peer);

    // Send acknowledgment with PWK + LWK back to client
    fpr_connect_t response = _make_fpr_info_with_keys(net, true, true, host_pwk, peer->security.lwk);
    esp_err_t err = fpr_node_send(net, peer_mac, &response, sizeof(response), FPR_PACKET_ID_CONTROL);

    if (err != ESP_OK) {
        peer->is_connected = false;
        peer->state = prev_state;
        peer->sec_state = FPR_SEC_STATE_PWK_SENT;
        return err;
    }

    ESP_LOGI(TAG, "Host: Peer connected with mutual keys: %s", peer->name);
    return err;
}

esp_err_t fpr_sec_client_handle_pwk(fpr_node_t *net, const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && peer != NULL && info != NULL, 
                        ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    
    ESP_LOGI(TAG, "Received PWK from host: %s", peer->name);
    memcpy(peer->security.pwk, info->pwk, FPR_KEY_SIZE);
    peer->security.pwk_valid = true;
    peer->sec_state = FPR_SEC_STATE_PWK_RECEIVED;
    
    // Generate client's own LWK (client contributes randomness)
    if (fpr_security_generate_lwk(peer->security.lwk) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate client LWK");
        return ESP_FAIL;
    }
    peer->security.lwk_valid = true;
    ESP_LOGI(TAG, "Generated client LWK");
    
    // Send device info back with PWK + client's LWK. Pre-arm LWK_SENT
    // before the send: a synchronous transport runs the host's
    // verify+ack reply inside fpr_node_send(), and the ack gate requires
    // sec_state==LWK_SENT — assigning after the send would both fail the
    // gate and clobber the ESTABLISHED state the nested ack set.
    peer->sec_state = FPR_SEC_STATE_LWK_SENT;
    fpr_connect_t response = _make_fpr_info_with_keys(net, true, true, peer->security.pwk, peer->security.lwk);
    esp_err_t err = fpr_node_send(net, peer_mac, &response, sizeof(response), FPR_PACKET_ID_CONTROL);

    if (err != ESP_OK) {
        peer->sec_state = FPR_SEC_STATE_PWK_RECEIVED;
        return err;
    }

    ESP_LOGI(TAG, "Sent PWK + LWK to host");
    return err;
}

esp_err_t fpr_sec_client_verify_ack(fpr_node_t *net, const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && peer != NULL && info != NULL, 
                        ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    
    // Verify PWK in acknowledgment
    if (!fpr_security_verify_pwk(info->pwk, peer->security.pwk)) {
        ESP_LOGW(TAG, "PWK verification failed in host ack");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Verify host echoed back our LWK correctly
    if (!fpr_security_verify_lwk(info->lwk, peer->security.lwk)) {
        ESP_LOGW(TAG, "LWK verification failed in host ack");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Received acknowledgment from host: %s", peer->name);
    
    // Mark as connected
    peer->is_connected = true;
    peer->state = FPR_PEER_STATE_CONNECTED;
    peer->sec_state = FPR_SEC_STATE_ESTABLISHED;

    // Fresh session: reset sequence tracking and drain stale packets
    _peer_clear_session(peer);

    ESP_LOGI(TAG, "Client: Connection established with %s (mutual keys)", peer->name);
    
    return ESP_OK;
}
