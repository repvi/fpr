/**
 * @file fpr_handler.c
 * @brief Version-aware packet handler dispatcher
 * 
 * Routes packets to appropriate handlers based on protocol version.
 * Supports legacy (older) and new (future) protocol versions.
 */

#include "fpr/fpr_handle.h"
#include "fpr/fpr_legacy.h"
#include "fpr/fpr_new.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <inttypes.h>

static const char *TAG = "fpr_handler";

/**
 * @brief Log version mismatch warning with details
 */
static void _log_version_info(code_version_t packet_version, const uint8_t *src_mac)
{
    ESP_LOGW(TAG, "Version info - Ours: %"PRId32".%"PRId32".%"PRId32", Theirs: %"PRId32".%"PRId32".%"PRId32" from " MACSTR,
             CODE_VERSION_MAJOR(FPR_PROTOCOL_VERSION),
             CODE_VERSION_MINOR(FPR_PROTOCOL_VERSION),
             CODE_VERSION_PATCH(FPR_PROTOCOL_VERSION),
             CODE_VERSION_MAJOR(packet_version),
             CODE_VERSION_MINOR(packet_version),
             CODE_VERSION_PATCH(packet_version),
             MAC2STR(src_mac));
}

bool fpr_version_handle_version(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len, code_version_t version) 
{
    // Check basic version compatibility
    if (!fpr_version_is_compatible(version)) {
        _log_version_info(version, esp_now_info->src_addr);
        ESP_LOGE(TAG, "Rejecting packet from " MACSTR " - incompatible version", MAC2STR(esp_now_info->src_addr));
        return false;
    }

    // Check if packet is current version (no special handling needed)
    if (fpr_version_is_current(version)) {
        return true;
    }
    
    // Version mismatch detected - log it
    _log_version_info(version, esp_now_info->src_addr);
    
    // Try legacy handler for older versions
    if (fpr_version_needs_legacy_handler(version)) {
        if (fpr_legacy_handle_protocol_version(version, esp_now_info, data, len)) {
            return true;
        }
    }
    
    // Try new handler for future versions
    if (fpr_version_needs_newer_handler(version)) {
        if (fpr_new_handle_protocol_version(version, esp_now_info, data, len)) {
            return true;
        }
    }
    
    ESP_LOGW(TAG, "No handler accepted packet from " MACSTR " with version %"PRId32".%"PRId32".%"PRId32,
             MAC2STR(esp_now_info->src_addr),
             CODE_VERSION_MAJOR(version),
             CODE_VERSION_MINOR(version),
             CODE_VERSION_PATCH(version));
    return false;
}