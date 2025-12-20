/**
 * @file fpr_legacy.c
 * @brief Legacy Protocol Handlers for Backward Compatibility
 * 
 * Processes packets from older firmware versions (v0 pre-versioning era).
 * Provides graceful degradation for networks with mixed firmware versions.
 * 
 * @version 1.0.0
 * @date December 2025
 */
#include "fpr/fpr_legacy.h"
#include "fpr/fpr_lts.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>

static const char *TAG = "fpr_legacy";

// Expected minimum packet size for v0 packets
// This should match the size of the packet structure from that era
#define FPR_V0_MIN_PACKET_SIZE 180  // Approximate, adjust based on actual v0 structure

bool fpr_legacy_is_v0_packet(const uint8_t *data, int len)
{
    if (data == NULL || len < (int)sizeof(uint32_t)) {
        return false;
    }
    
    // V0 packets have version field as 0 (or uninitialized)
    // The version field location depends on your packet structure
    // For now, assume it's detectable by the version being 0
    return true;  // Caller should check version field directly
}

bool fpr_legacy_handle_protocol_v0(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    if (esp_now_info == NULL || data == NULL) {
        ESP_LOGE(TAG, "Invalid arguments to legacy handler");
        return false;
    }
    
    ESP_LOGW(TAG, "Processing legacy (v0) packet from " MACSTR " - len: %d",
             MAC2STR(esp_now_info->src_addr), len);
    
    // Basic size check - v0 packets should still meet minimum size
    if (len < FPR_V0_MIN_PACKET_SIZE) {
        ESP_LOGW(TAG, "Legacy packet too small: %d < %d", len, FPR_V0_MIN_PACKET_SIZE);
        return false;
    }
    
    // ========== V0 -> V1 MIGRATION ==========
    // For v0 -> v1: Protocol structure is identical, just version wasn't set.
    // Return true to indicate packet can be processed with current handler.
    //
    // FUTURE: When v2 is released with breaking changes, add conversion here:
    //
    // typedef struct {
    //     // Old v0/v1 packet structure fields
    // } fpr_package_v0_t;
    //
    // fpr_package_v0_t *old_pkg = (fpr_package_v0_t *)data;
    // 
    // // Convert to current format:
    // fpr_package_t new_pkg = {0};
    // new_pkg.field1 = old_pkg->old_field1;
    // new_pkg.field2 = convert_old_to_new(old_pkg->old_field2);
    // // ... etc
    //
    // // Process the converted packet
    // return process_converted_packet(&new_pkg);
    
    return true;
}

bool fpr_legacy_handle_protocol_version(code_version_t version, const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) 
{
    // Handle legacy v0 packets (version=0)
    return false;
}