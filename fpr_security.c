/**
 * @file fpr_security.c
 * @brief FPR Security and Key Exchange Implementation
 * 
 * Implements cryptographic key generation and verification for the
 * FPR security handshake protocol.
 * 
 * @version 1.0.0
 * @date December 2025
 */

#include "fpr/fpr_security.h"
#include "esp_random.h"
#include "esp_check.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "fpr_security";

esp_err_t fpr_security_generate_pwk(uint8_t *pwk_out)
{
    ESP_RETURN_ON_FALSE(pwk_out != NULL, ESP_ERR_INVALID_ARG, TAG, "PWK output buffer is NULL");
    
    // Generate 16 bytes of cryptographically secure random data
    esp_fill_random(pwk_out, FPR_KEY_SIZE);
    
    ESP_LOGI(TAG, "Generated new PWK");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, pwk_out, FPR_KEY_SIZE, ESP_LOG_DEBUG);
    
    return ESP_OK;
}

esp_err_t fpr_security_generate_lwk(uint8_t *lwk_out)
{
    ESP_RETURN_ON_FALSE(lwk_out != NULL, ESP_ERR_INVALID_ARG, TAG, "LWK output buffer is NULL");
    
    // Generate 16 bytes of cryptographically secure random data
    esp_fill_random(lwk_out, FPR_KEY_SIZE);
    
    ESP_LOGI(TAG, "Generated new LWK");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, lwk_out, FPR_KEY_SIZE, ESP_LOG_DEBUG);
    
    return ESP_OK;
}

bool fpr_security_verify_pwk(const uint8_t *pwk_received, const uint8_t *pwk_expected)
{
    if (!pwk_received || !pwk_expected) {
        return false;
    }
    
    bool match = (memcmp(pwk_received, pwk_expected, FPR_KEY_SIZE) == 0);
    
    if (!match) {
        ESP_LOGW(TAG, "PWK verification failed");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, pwk_received, FPR_KEY_SIZE, ESP_LOG_DEBUG);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, pwk_expected, FPR_KEY_SIZE, ESP_LOG_DEBUG);
    } else {
        ESP_LOGD(TAG, "PWK verified successfully");
    }
    
    return match;
}

bool fpr_security_verify_lwk(const uint8_t *lwk_received, const uint8_t *lwk_expected)
{
    if (!lwk_received || !lwk_expected) {
        return false;
    }
    
    bool match = (memcmp(lwk_received, lwk_expected, FPR_KEY_SIZE) == 0);
    
    if (!match) {
        ESP_LOGW(TAG, "LWK verification failed");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, lwk_received, FPR_KEY_SIZE, ESP_LOG_DEBUG);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, lwk_expected, FPR_KEY_SIZE, ESP_LOG_DEBUG);
    } else {
        ESP_LOGD(TAG, "LWK verified successfully");
    }
    
    return match;
}

bool fpr_security_is_fully_established(const fpr_security_keys_t *keys)
{
    if (!keys) {
        return false;
    }
    
    return (keys->pwk_valid && keys->lwk_valid);
}

void fpr_security_init_keys(fpr_security_keys_t *keys)
{
    if (keys) {
        memset(keys, 0, sizeof(fpr_security_keys_t));
        keys->pwk_valid = false;
        keys->lwk_valid = false;
    }
}

void fpr_security_clear_keys(fpr_security_keys_t *keys)
{
    if (keys) {
        // Secure wipe - overwrite with random data first, then zeros
        esp_fill_random(keys->pwk, FPR_KEY_SIZE);
        esp_fill_random(keys->lwk, FPR_KEY_SIZE);
        memset(keys, 0, sizeof(fpr_security_keys_t));
        keys->pwk_valid = false;
        keys->lwk_valid = false;
        ESP_LOGD(TAG, "Security keys cleared");
    }
}
