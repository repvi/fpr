/**
 * @file fpr_lts.c
 * @brief FPR Long-Term Support (LTS) implementation
 * 
 * This module provides version management and compatibility checking
 * for the FPR protocol. It ensures backward and forward compatibility
 * across different protocol versions.
 * 
 * @version 1.0.0
 * @date December 2024
 * 
 * LTS Policy:
 * - Version 1.0.0 is the first LTS release
 * - LTS versions receive security updates for 2+ years
 * - Breaking changes only in major version bumps
 * - Minor versions add features maintaining backward compatibility
 * - Patch versions are bug fixes only
 */

#include "fpr/fpr_lts.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "fpr_lts";

/**
 * @brief Get human-readable version string
 * @param version Version to convert
 * @param buf Buffer to store string
 * @param buf_size Size of buffer
 */
void fpr_lts_version_to_string(code_version_t version, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return;
    }
    
    uint8_t major = CODE_VERSION_MAJOR(version);
    uint8_t minor = CODE_VERSION_MINOR(version);
    uint8_t patch = CODE_VERSION_PATCH(version);
    
    snprintf(buf, buf_size, "%u.%u.%u", major, minor, patch);
}

/**
 * @brief Log version compatibility information
 * @param remote_version Version to check
 */
void fpr_lts_log_compatibility(code_version_t remote_version)
{
    char local_str[16];
    char remote_str[16];
    
    fpr_lts_version_to_string(FPR_PROTOCOL_VERSION, local_str, sizeof(local_str));
    fpr_lts_version_to_string(remote_version, remote_str, sizeof(remote_str));
    
    if (fpr_version_is_compatible(remote_version)) {
        if (fpr_version_is_current(remote_version)) {
            ESP_LOGI(TAG, "Remote version %s is compatible (same major version as local %s)", 
                     remote_str, local_str);
        } else if (fpr_version_needs_legacy_handler(remote_version)) {
            ESP_LOGW(TAG, "Remote version %s requires legacy handler (local: %s)", 
                     remote_str, local_str);
        } else if (fpr_version_needs_newer_handler(remote_version)) {
            ESP_LOGW(TAG, "Remote version %s is newer than local %s - limited compatibility", 
                     remote_str, local_str);
        }
    } else {
        ESP_LOGE(TAG, "Remote version %s is incompatible with local %s", 
                 remote_str, local_str);
    }
}

/**
 * @brief Get the minimum supported protocol version
 * @return Minimum supported version
 */
code_version_t fpr_lts_get_min_supported_version(void)
{
    return FPR_MIN_SUPPORTED_VERSION;
}

/**
 * @brief Check if a feature is supported by a given version
 * @param version Version to check
 * @param feature Feature identifier (use FPR_SUPPORTS_* macros for known features)
 * @return true if feature is supported
 */
bool fpr_lts_supports_feature(code_version_t version, const char *feature)
{
    if (feature == NULL) {
        return false;
    }
    
    // Check known features
    if (strcmp(feature, "fragmentation") == 0) {
        return FPR_SUPPORTS_FRAGMENTATION(version);
    }
    if (strcmp(feature, "mesh_routing") == 0) {
        return FPR_SUPPORTS_MESH_ROUTING(version);
    }
    if (strcmp(feature, "versioning") == 0) {
        return FPR_HAS_VERSIONING(version);
    }
    
    // Unknown feature - assume not supported for safety
    return false;
}