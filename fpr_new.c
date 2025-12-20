/**
 * @file fpr_new.c
 * @brief Future Protocol Version Handlers for Forward Compatibility
 * 
 * Handles packets from newer protocol versions. Provides forward compatibility
 * by attempting to extract compatible fields from future packet formats.
 * 
 * @version 1.0.0
 * @date December 2025
 */

#include "fpr/fpr_new.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <inttypes.h>

static const char *TAG = "fpr_new";

bool fpr_new_handle_protocol_version(code_version_t version, const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    (void)data;
    (void)len;
    
    // Log the newer version info
    ESP_LOGW(TAG, "Received packet from future version %"PRId32".%"PRId32".%"PRId32" from " MACSTR,
             CODE_VERSION_MAJOR(version),
             CODE_VERSION_MINOR(version),
             CODE_VERSION_PATCH(version),
             MAC2STR(esp_now_info->src_addr));
    
    // For now, reject future versions as we don't know how to handle them
    // When future protocol versions are defined, add handlers here
    
    ESP_LOGI(TAG, "No handler available for future protocol version - dropping packet");
    return false;
}
