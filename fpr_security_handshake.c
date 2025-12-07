/**
 * @file fpr_security_handshake.c
 * @brief FPR Security Handshake Helper Functions
 * 
 * Common helper functions for host and client security handshakes
 */

#include "fpr/fpr_security_handshake.h"
#include "fpr/fpr_security.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "fpr_sec_handshake";

extern fpr_connect_t make_fpr_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk);
extern esp_err_t fpr_network_send_to_peer(uint8_t *peer_address, void *data, int size, fpr_package_id_t package_id);

esp_err_t fpr_sec_host_send_pwk(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const uint8_t *host_pwk)
{
    ESP_RETURN_ON_FALSE(peer_mac != NULL && peer != NULL && host_pwk != NULL, 
                        ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    
    ESP_LOGI(TAG, "Sending PWK to client: %s", peer->name);
    fpr_connect_t response = make_fpr_info_with_keys(true, false, host_pwk, NULL);
    esp_err_t err = fpr_network_send_to_peer((uint8_t *)peer_mac, &response, sizeof(response), FPR_PACKET_ID_CONTROL);
    
    if (err == ESP_OK) {
        peer->sec_state = FPR_SEC_STATE_PWK_SENT;
        memcpy(peer->security.pwk, host_pwk, FPR_KEY_SIZE);
        peer->security.pwk_valid = true;
    }
    
    return err;
}

esp_err_t fpr_sec_host_verify_and_ack(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info, const uint8_t *host_pwk)
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
    
    // Send acknowledgment with PWK + LWK back to client
    fpr_connect_t response = make_fpr_info_with_keys(true, true, host_pwk, peer->security.lwk);
    esp_err_t err = fpr_network_send_to_peer((uint8_t *)peer_mac, &response, sizeof(response), FPR_PACKET_ID_CONTROL);
    
    if (err == ESP_OK) {
        peer->sec_state = FPR_SEC_STATE_LWK_SENT;
        // Mark as connected on host side
        peer->is_connected = true;
        peer->state = FPR_PEER_STATE_CONNECTED;
        peer->sec_state = FPR_SEC_STATE_ESTABLISHED;
        
        // Reset sequence tracking for new session (handles peer restarts)
        peer->last_seq_num = 0;
        peer->receiving_fragmented = false;
        peer->fragment_seq_num = 0;
        
        // Drain any stale queued packets from previous session
        if (peer->response_queue != NULL) {
            xQueueReset(peer->response_queue);
            fpr_package_t tmp;
            peer->queued_packets = 0;
        }
        
        ESP_LOGI(TAG, "Host: Peer connected with mutual keys: %s", peer->name);
    }
    
    return err;
}

esp_err_t fpr_sec_client_handle_pwk(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info)
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
    
    // Send device info back with PWK + client's LWK
    fpr_connect_t response = make_fpr_info_with_keys(true, true, peer->security.pwk, peer->security.lwk);
    esp_err_t err = fpr_network_send_to_peer((uint8_t *)peer_mac, &response, sizeof(response), FPR_PACKET_ID_CONTROL);
    
    if (err == ESP_OK) {
        peer->sec_state = FPR_SEC_STATE_LWK_SENT;
        ESP_LOGI(TAG, "Sent PWK + LWK to host");
    }
    
    return err;
}

esp_err_t fpr_sec_client_verify_ack(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info)
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
    
    // Reset sequence tracking for new session (handles host restarts)
    peer->last_seq_num = 0;
    peer->receiving_fragmented = false;
    peer->fragment_seq_num = 0;
    
    // Drain any stale queued packets from previous session
    if (peer->response_queue != NULL) {
        fpr_package_t tmp;
        while (xQueueReceive(peer->response_queue, &tmp, 0) == pdPASS) { /* drop stale packets */ }
        peer->queued_packets = 0;
    }
    
    ESP_LOGI(TAG, "Client: Connection established with %s (mutual keys)", peer->name);
    
    return ESP_OK;
}
