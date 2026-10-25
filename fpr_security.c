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

static esp_err_t _gen_key(uint8_t *out, const char *which)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "%s output buffer is NULL", which);
    esp_fill_random(out, FPR_KEY_SIZE);
    ESP_LOGI(TAG, "Generated new %s", which);
    return ESP_OK;
}

esp_err_t fpr_security_generate_pwk(uint8_t *pwk_out)
{
    return _gen_key(pwk_out, "PWK");
}

esp_err_t fpr_security_generate_lwk(uint8_t *lwk_out)
{
    return _gen_key(lwk_out, "LWK");
}

static bool _verify_key(const uint8_t *received, const uint8_t *expected, const char *which)
{
    if (!received || !expected) {
        return false;
    }
    bool match = (memcmp(received, expected, FPR_KEY_SIZE) == 0);
    if (!match) {
        ESP_LOGW(TAG, "%s verification failed", which);
    } else {
        ESP_LOGD(TAG, "%s verified successfully", which);
    }
    return match;
}

bool fpr_security_verify_pwk(const uint8_t *pwk_received, const uint8_t *pwk_expected)
{
    return _verify_key(pwk_received, pwk_expected, "PWK");
}

bool fpr_security_verify_lwk(const uint8_t *lwk_received, const uint8_t *lwk_expected)
{
    return _verify_key(lwk_received, lwk_expected, "LWK");
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
    }
}

void fpr_security_clear_keys(fpr_security_keys_t *keys)
{
    if (keys) {
        // Secure wipe - overwrite with random data first, then zeros
        esp_fill_random(keys->pwk, FPR_KEY_SIZE);
        esp_fill_random(keys->lwk, FPR_KEY_SIZE);
        memset(keys, 0, sizeof(fpr_security_keys_t));
        ESP_LOGD(TAG, "Security keys cleared");
    }
}
