#pragma once

/**
 * @file fpr_security.h
 * @brief FPR Security and Key Exchange System (WiFi-style 4-way handshake)
 * 
 * Implements a secure key exchange protocol similar to WiFi WPA2:
 * 1. Host generates PWK (Primary Working Key) - like WiFi's PMK
 * 2. Host broadcasts device info with PWK
 * 3. Client receives PWK, stores it
 * 4. Client generates its own LWK (Local Working Key) - client's random contribution
 * 5. Client sends device info with PWK + LWK to host
 * 6. Host receives, verifies PWK, stores client's LWK
 * 7. Host sends acknowledgment with PWK + LWK back to client
 * 8. Both mark as connected - both parties contributed randomness
 * 
 * This provides:
 * - Mutual authentication (both verify PWK)
 * - Both parties contribute to session key (LWK from client)
 * - Protection against replay attacks
 * - Reconnection capability using stored keys
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FPR_KEY_SIZE 16  // 128-bit keys

/**
 * @brief Security key structure for FPR peer authentication
 */
typedef struct {
    uint8_t pwk[FPR_KEY_SIZE];  // Primary Working Key (host-generated)
    uint8_t lwk[FPR_KEY_SIZE];  // Local Working Key (session-specific)
    bool pwk_valid;              // PWK has been set
    bool lwk_valid;              // LWK has been set
} fpr_security_keys_t;

/**
 * @brief Security context for connection state
 */
typedef enum {
    FPR_SEC_STATE_NONE = 0,        // No security established
    FPR_SEC_STATE_PWK_SENT,        // Host sent PWK
    FPR_SEC_STATE_PWK_RECEIVED,    // Client received PWK
    FPR_SEC_STATE_LWK_SENT,        // Host sent LWK
    FPR_SEC_STATE_LWK_RECEIVED,    // Client received LWK
    FPR_SEC_STATE_ESTABLISHED      // Full handshake complete
} fpr_security_state_t;

/**
 * @brief Generate a new Primary Working Key (PWK) for host
 * @param pwk_out Buffer to store generated PWK (must be FPR_KEY_SIZE bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_security_generate_pwk(uint8_t *pwk_out);

/**
 * @brief Generate a new Local Working Key (LWK) for a peer session
 * @param lwk_out Buffer to store generated LWK (must be FPR_KEY_SIZE bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_security_generate_lwk(uint8_t *lwk_out);

/**
 * @brief Verify if a PWK matches the expected value
 * @param pwk_received Received PWK to verify
 * @param pwk_expected Expected PWK value
 * @return true if keys match, false otherwise
 */
bool fpr_security_verify_pwk(const uint8_t *pwk_received, const uint8_t *pwk_expected);

/**
 * @brief Verify if an LWK matches the expected value
 * @param lwk_received Received LWK to verify
 * @param lwk_expected Expected LWK value
 * @return true if keys match, false otherwise
 */
bool fpr_security_verify_lwk(const uint8_t *lwk_received, const uint8_t *lwk_expected);

/**
 * @brief Check if both keys are valid for a secure connection
 * @param keys Security keys structure to check
 * @return true if both PWK and LWK are valid
 */
bool fpr_security_is_fully_established(const fpr_security_keys_t *keys);

/**
 * @brief Initialize security keys structure to empty state
 * @param keys Security keys structure to initialize
 */
void fpr_security_init_keys(fpr_security_keys_t *keys);

/**
 * @brief Clear all security keys from memory (secure wipe)
 * @param keys Security keys structure to clear
 */
void fpr_security_clear_keys(fpr_security_keys_t *keys);

#ifdef __cplusplus
}
#endif
