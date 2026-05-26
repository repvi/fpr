#pragma once

/**
 * @file helpers.h
 * @brief Sprout Internal Helper Functions and Macros
 *
 * Internal utility functions for peer management, data handling,
 * and common operations. These are implementation details not part
 * of the public API.
 *
 * @warning Internal API - subject to change without notice.
 */

#include "sprout/internal/private_defs.h"
#include "esp_timer.h"

static inline void _safe_string_copy(char *dest, const char *src, size_t dest_size)
{
    if (dest && src && dest_size > 0) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

static inline SPROUT_STORE_HASH_TYPE *_get_peer_from_map(const uint8_t *peer_mac)
{
    return (SPROUT_STORE_HASH_TYPE *)hashmap_get(sprout_net.peers_map, peer_mac);
}

static inline void _update_peer_rssi_and_timestamp(SPROUT_STORE_HASH_TYPE *peer, const esp_now_recv_info_t *esp_now_info)
{
    if (peer && esp_now_info) {
        peer->last_seen = esp_timer_get_time();
        peer->rssi = esp_now_info->rx_ctrl->rssi;
    }
}

static inline uint32_t _sprout_get_power_adjusted_interval(uint32_t base_interval_ms)
{
    if (sprout_net.power_mode == SPROUT_POWER_LOW) {
        return base_interval_ms * SPROUT_LOW_POWER_MULTIPLIER;
    }
    return base_interval_ms;
}

static inline bool is_broadcast_address(const uint8_t *mac)
{
    const uint8_t broadcast_addr[6] = SPROUT_BROADCAST_ADDRESS;
    return (memcmp(mac, broadcast_addr, 6) == 0);
}

static inline bool is_address_broadcast(const uint8_t *mac)
{
    return is_broadcast_address(mac);
}

static inline bool is_sprout_package_compatible(int len)
{
    return (len == sizeof(sprout_package_t));
}

static inline void sprout_set_peer_info(esp_now_peer_info_t *gen_info)
{
    gen_info->channel = 0;
    gen_info->encrypt = false;
    gen_info->ifidx = WIFI_IF_STA;
}

void _store_data_from_peer_helper(const esp_now_recv_info_t *esp_now_info, const sprout_package_t *data);

esp_err_t _add_peer_internal(uint8_t *peer_mac, const char *name, bool is_connected, uint32_t key);

esp_err_t _remove_peer_internal(uint8_t *peer_mac);

void _cleanup_peer_entry(void *key, void *value, void *user_data);

void _reset_all_peers(void);

esp_err_t _add_discovered_peer(const char *name, uint8_t *address, uint32_t key, bool is_connected);

void _copy_peer_to_info(const SPROUT_STORE_HASH_TYPE *peer, sprout_peer_info_t *info);

sprout_connect_t make_sprout_info_with_keys(bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk);
