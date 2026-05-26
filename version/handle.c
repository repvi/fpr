/**
 * @file sprout_handle.c
 * @brief Version-Aware Packet Handler Dispatcher
 * 
 * Routes incoming packets to appropriate handlers based on protocol version.
 * Supports legacy (v0), current (v1.x), and future protocol versions.
 * 
 * @version 1.0.0
 * @date December 2025
 */

#include "sprout/sprout_handle.h"
#include "sprout/sprout_legacy.h"
#include "sprout/sprout_new.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <inttypes.h>

static const char *TAG = "sprout_handler";

/**
 * @brief Log version mismatch warning with details
 */
static void _log_version_info(code_version_t packet_version, const uint8_t *src_mac)
{
    ESP_LOGW(TAG, "Version info - Ours: %"PRId32".%"PRId32".%"PRId32", Theirs: %"PRId32".%"PRId32".%"PRId32" from " MACSTR,
             CODE_VERSION_MAJOR(SPROUT_PROTOCOL_VERSION),
             CODE_VERSION_MINOR(SPROUT_PROTOCOL_VERSION),
             CODE_VERSION_PATCH(SPROUT_PROTOCOL_VERSION),
             CODE_VERSION_MAJOR(packet_version),
             CODE_VERSION_MINOR(packet_version),
             CODE_VERSION_PATCH(packet_version),
             MAC2STR(src_mac));
}

bool sprout_version_handle_version(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len, code_version_t version) 
{
    // Check basic version compatibility
    if (!sprout_version_is_compatible(version)) {
        _log_version_info(version, esp_now_info->src_addr);
        ESP_LOGE(TAG, "Rejecting packet from " MACSTR " - incompatible version", MAC2STR(esp_now_info->src_addr));
        return false;
    }

    // Check if packet is current version (no special handling needed)
    if (sprout_version_is_current(version)) {
        return true;
    }
    
    // Version mismatch detected - log it
    _log_version_info(version, esp_now_info->src_addr);
    
    // Try legacy handler for older versions
    if (sprout_version_needs_legacy_handler(version)) {
        if (sprout_legacy_handle_protocol_version(version, esp_now_info, data, len)) {
            return true;
        }
    }
    
    // Try new handler for future versions
    if (sprout_version_needs_newer_handler(version)) {
        if (sprout_new_handle_protocol_version(version, esp_now_info, data, len)) {
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