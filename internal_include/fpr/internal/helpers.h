#pragma once

/**
 * @file helpers.h
 * @brief FPR Internal Helper Functions and Macros
 * 
 * Internal utility functions for peer management, data handling,
 * and common operations. These are implementation details not part
 * of the public API.
 * 
 * @warning Internal API - subject to change without notice.
 * 
 * @version 1.0.0
 * @date December 2024
 */

#include "fpr/internal/private_defs.h"
#include "esp_timer.h"

// Helper: Safe string copy with NUL termination
static inline void _safe_string_copy(char *dest, const char *src, size_t dest_size)
{
    if (dest && src && dest_size > 0) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

// Helper: Get peer from hashmap with type cast
static inline FPR_STORE_HASH_TYPE *_get_peer_from_map(const uint8_t *peer_mac)
{
    return (FPR_STORE_HASH_TYPE *)hashmap_get(&fpr_net.peers_map, peer_mac);
}

// Helper: Update peer RSSI and timestamp from ESP-NOW info
static inline void _update_peer_rssi_and_timestamp(FPR_STORE_HASH_TYPE *peer, const esp_now_recv_info_t *esp_now_info)
{
    if (peer && esp_now_info) {
        peer->last_seen = esp_timer_get_time();
        /* rx_ctrl is driver-provided but not documented as always
         * non-NULL; treat a missing block as "RSSI unknown". */
        peer->rssi = esp_now_info->rx_ctrl ? esp_now_info->rx_ctrl->rssi : 0;
    }
}

// Helper: Get interval adjusted for power mode
static inline uint32_t _fpr_get_power_adjusted_interval(uint32_t base_interval_ms)
{
    if (fpr_net.power_mode == FPR_POWER_LOW) {
        if (base_interval_ms > UINT32_MAX / FPR_LOW_POWER_MULTIPLIER) {
            return UINT32_MAX;
        }
        return base_interval_ms * FPR_LOW_POWER_MULTIPLIER;
    }
    return base_interval_ms;
}

static inline bool is_broadcast_address(const uint8_t *mac)
{
    if (!mac) {
        return false;
    }
    const uint8_t broadcast_addr[6] = FPR_BROADCAST_ADDRESS;
    return (memcmp(mac, broadcast_addr, 6) == 0);
}

// should be global function
static inline bool is_address_broadcast(const uint8_t *mac)
{
    return is_broadcast_address(mac);
}

static inline bool is_fpr_package_compatible(int len)
{
    return (len == sizeof(fpr_package_t));
}

static inline void fpr_set_peer_info(esp_now_peer_info_t *gen_info)
{
    if (!gen_info) {
        return;
    }
    gen_info->channel = 0; // Use current channel
    gen_info->encrypt = false; // No encryption
    gen_info->ifidx = WIFI_IF_STA;
}

/* Reset a peer to "known but not connected". Stale handshake state must
 * go with the connection: a leftover sec_state/keys pair locks out
 * re-handshakes (the fresh-request paths require SEC_STATE_NONE), and a
 * stale watermark/fragment buffer would drop the new session's traffic. */
static inline void _reset_peer_connection(FPR_STORE_HASH_TYPE *peer, fpr_peer_state_t state)
{
    if (!peer) {
        return;
    }
    peer->is_connected = false;
    peer->state = state;
    peer->sec_state = FPR_SEC_STATE_NONE;
    fpr_security_clear_keys(&peer->security);
    peer->last_seq_num = 0;
    peer->receiving_fragmented = false;
    peer->fragment_seq_num = 0;
}

void _store_data_from_peer_helper(const esp_now_recv_info_t *esp_now_info, const fpr_package_t *data);

esp_err_t _add_peer_internal(uint8_t *peer_mac, const char *name, bool is_connected, uint32_t key);

esp_err_t _add_discovered_peer(const char *name, uint8_t *address, uint32_t key, bool is_connected);

void _copy_peer_to_info(const FPR_STORE_HASH_TYPE *peer, fpr_peer_info_t *info);
fpr_connect_t make_fpr_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk);
