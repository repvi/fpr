#pragma once

/**
 * @file fpr_security_handshake.h
 * @brief FPR Security Handshake Helper Functions (WiFi-style)
 * 
 * Implements WiFi WPA2-style 4-way handshake:
 * 1. Host sends PWK (like PMK in WiFi)
 * 2. Client generates LWK and sends PWK+LWK back
 * 3. Host verifies, stores client's LWK, sends acknowledgment
 * 4. Client verifies acknowledgment, both are connected
 * 
 * This ensures mutual authentication and both parties contribute randomness.
 */

#include "fpr/internal/private_defs.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Host: Send PWK to client (Step 1)
 * @param peer_mac Client MAC address
 * @param peer Client peer structure
 * @param host_pwk Host's PWK to send
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_sec_host_send_pwk(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const uint8_t *host_pwk);

/**
 * @brief Host: Verify client's PWK+LWK and send acknowledgment (Step 3)
 * Verifies PWK, stores client's LWK, sends ack, marks connected
 * @param peer_mac Client MAC address
 * @param peer Client peer structure
 * @param info Connection info from client (contains PWK + client's LWK)
 * @param host_pwk Host's PWK for verification
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_sec_host_verify_and_ack(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info, const uint8_t *host_pwk);

/**
 * @brief Client: Handle PWK received from host (Step 2)
 * Stores PWK, generates own LWK, sends PWK+LWK back to host
 * @param peer_mac Host MAC address
 * @param peer Host peer structure
 * @param info Connection info from host (contains PWK)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_sec_client_handle_pwk(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info);

/**
 * @brief Client: Verify acknowledgment from host (Step 4)
 * Verifies host echoed back PWK+LWK correctly, marks connected
 * @param peer_mac Host MAC address
 * @param peer Host peer structure
 * @param info Connection info from host (acknowledgment with PWK+LWK)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_sec_client_verify_ack(const uint8_t *peer_mac, FPR_STORE_HASH_TYPE *peer, const fpr_connect_t *info);

#ifdef __cplusplus
}
#endif
