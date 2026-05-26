#pragma once

/**
 * @file sprout_security.h
 * @brief FPR Security and Key Exchange System
 * 
 * Implements a WiFi WPA2-style 4-way handshake for secure peer authentication:
 * 
 * Handshake Flow:
 * 1. Host generates PWK (Primary Working Key) - similar to WiFi's PMK
 * 2. Host sends device info with PWK to discovered client
 * 3. Client receives PWK, stores it, generates own LWK (Local Working Key)
 * 4. Client sends device info with PWK + LWK back to host
 * 5. Host receives, verifies PWK, stores client's LWK
 * 6. Host sends acknowledgment with PWK + LWK back to client
 * 7. Client verifies acknowledgment
 * 8. Both mark connection as ESTABLISHED
 * 
 * Security Properties:
 * - Mutual authentication (both parties verify PWK)
 * - Both parties contribute randomness to session (LWK from client)
 * - Protection against replay attacks via sequence numbers
 * - Reconnection capability using stored keys
 * - Keys are stored per-peer for session isolation
 * 
 * @version 1.0.0
 * @date December 2025
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPROUT_KEY_SIZE 16  // 128-bit keys

/**
 * @brief Security key structure for FPR peer authentication
 */
typedef struct {
    uint8_t pwk[SPROUT_KEY_SIZE];  // Primary Working Key (host-generated)
    uint8_t lwk[SPROUT_KEY_SIZE];  // Local Working Key (session-specific)
    bool pwk_valid;              // PWK has been set
    bool lwk_valid;              // LWK has been set
} sprout_security_keys_t;

/**
 * @brief Security context for connection state
 */
typedef enum {
    SPROUT_SEC_STATE_NONE = 0,        // No security established
    SPROUT_SEC_STATE_PWK_SENT,        // Host sent PWK
    SPROUT_SEC_STATE_PWK_RECEIVED,    // Client received PWK
    SPROUT_SEC_STATE_LWK_SENT,        // Host sent LWK
    SPROUT_SEC_STATE_LWK_RECEIVED,    // Client received LWK
    SPROUT_SEC_STATE_ESTABLISHED      // Full handshake complete
} sprout_security_state_t;

/**
 * @brief Generate a new Primary Working Key (PWK) for host
 * @param pwk_out Buffer to store generated PWK (must be SPROUT_KEY_SIZE bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sprout_security_generate_pwk(uint8_t *pwk_out);

/**
 * @brief Generate a new Local Working Key (LWK) for a peer session
 * @param lwk_out Buffer to store generated LWK (must be SPROUT_KEY_SIZE bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t sprout_security_generate_lwk(uint8_t *lwk_out);

/**
 * @brief Verify if a PWK matches the expected value
 * @param pwk_received Received PWK to verify
 * @param pwk_expected Expected PWK value
 * @return true if keys match, false otherwise
 */
bool sprout_security_verify_pwk(const uint8_t *pwk_received, const uint8_t *pwk_expected);

/**
 * @brief Verify if an LWK matches the expected value
 * @param lwk_received Received LWK to verify
 * @param lwk_expected Expected LWK value
 * @return true if keys match, false otherwise
 */
bool sprout_security_verify_lwk(const uint8_t *lwk_received, const uint8_t *lwk_expected);

/**
 * @brief Check if both keys are valid for a secure connection
 * @param keys Security keys structure to check
 * @return true if both PWK and LWK are valid
 */
bool sprout_security_is_fully_established(const sprout_security_keys_t *keys);

/**
 * @brief Initialize security keys structure to empty state
 * @param keys Security keys structure to initialize
 */
void sprout_security_init_keys(sprout_security_keys_t *keys);

/**
 * @brief Clear all security keys from memory (secure wipe)
 * @param keys Security keys structure to clear
 */
void sprout_security_clear_keys(sprout_security_keys_t *keys);

#ifdef __cplusplus
}
#endif
