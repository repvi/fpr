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
#include "esp_heap_caps.h"

// Helper: Safe string copy with NUL termination
static inline void _safe_string_copy(char *dest, const char *src, size_t dest_size)
{
    if (dest && src && dest_size > 0) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

// Helper: Get peer from hashmap with type cast
static inline FPR_STORE_HASH_TYPE *_peer_from_map(fpr_node_t *net, const uint8_t *peer_mac)
{
    return (FPR_STORE_HASH_TYPE *)hashmap_get(&net->peers_map, peer_mac);
}

/* Transports that cannot measure RSSI report 0 ("unknown"). */
static inline int8_t _rx_rssi(const fpr_rx_frame_t *frame)
{
    return frame ? frame->rssi : 0;
}

// Helper: Update peer RSSI and timestamp from the received frame
static inline void _peer_touch(FPR_STORE_HASH_TYPE *peer, const fpr_rx_frame_t *frame)
{
    if (peer && frame) {
        peer->last_seen = esp_timer_get_time();
        peer->rssi = _rx_rssi(frame);
    }
}

// Helper: Get interval adjusted for power mode
static inline uint32_t _power_interval(fpr_node_t *net, uint32_t base_interval_ms)
{
    if (net->power_mode == FPR_POWER_LOW) {
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

static inline bool is_fpr_package_compatible(int len)
{
    return (len == sizeof(fpr_package_t));
}

static inline void fpr_set_peer_info(fpr_wire_peer_info_t *gen_info)
{
    if (!gen_info) {
        return;
    }
    gen_info->channel = 0; // Use current channel
    gen_info->encrypt = false; // No encryption
    gen_info->ifidx = 0;       // STA (wire value; not a driver ifidx)
}

/* Serialize access to net->peers_map and the peer stores it owns.
 * ESP-NOW RX callbacks (WiFi task), the loop/reconnect tasks and
 * application calls all reach the map; without this a concurrent remove
 * can free a store another context is using. The lock is recursive so
 * callbacks invoked while a caller holds it (on_data calling
 * fpr_remove_peer, request_cb calling fpr_host_approve)
 * re-enter safely on the same task. */
static inline void _peers_lock(fpr_node_t *net)
{
    if (net->peers_lock != NULL) {
        xSemaphoreTakeRecursive(net->peers_lock, portMAX_DELAY);
    }
}

static inline void _peers_unlock(fpr_node_t *net)
{
    if (net->peers_lock != NULL) {
        xSemaphoreGiveRecursive(net->peers_lock);
    }
}

/* Lock + map lookup in one step for the common
 * lock → _peer_from_map → NULL-check preamble. The caller MUST
 * _peers_unlock() on every path, including NULL (the helper leaves
 * the lock held either way). */
static inline FPR_STORE_HASH_TYPE *_peer_lookup_locked(fpr_node_t *net, const uint8_t *peer_mac)
{
    _peers_lock(net);
    return _peer_from_map(net, peer_mac);
}

/* Snapshot the radio port. Teardown memsets the node without holding
 * the lock, so a bare net->transport->op() can dereference NULL
 * mid-teardown — but every transport vtable is a static const, so a
 * captured non-NULL pointer stays callable even while the node is
 * being zeroed. Callers still NULL-check the snapshot. */
static inline const fpr_transport_t *_transport(fpr_node_t *net)
{
    return net->transport;
}

/* Count one transport send() outcome under peers_lock. Safe to call while
 * already holding the lock (it is recursive) — the extender path counts
 * from inside the RX critical section. */
static inline void _stats_count_send(fpr_node_t *net, esp_err_t err)
{
    _peers_lock(net);
    if (err == ESP_OK) {
        net->stats.packets_sent++;
    } else {
        net->stats.send_failures++;
    }
    _peers_unlock(net);
}

/* Free-or-defer for a store already detached (or being detached) from
 * the map: a blocking reader may hold a reference, in which case the
 * store becomes `dead` and is freed by _peer_store_release(). Call only
 * under _peers_lock(). Replaces every bare vQueueDelete+clear_keys+free
 * sequence so no free site can outrun a live reader. */
static inline void _peer_store_free_or_defer(FPR_STORE_HASH_TYPE *peer)
{
    if (!peer) {
        return;
    }
    if (peer->refcount > 0) {
        peer->dead = true;
        return;
    }
    if (peer->response_queue != NULL) {
        vQueueDelete(peer->response_queue);
    }
    fpr_security_clear_keys(&peer->security);
    heap_caps_free(peer);
}

/* Acquire a temporary reference on the store for `mac`. The reference
 * keeps the store (and its response queue) alive after _peers_unlock(),
 * so a caller may block on the queue without holding the peers lock —
 * holding it would starve the RX path that fills the queue. Pair every
 * acquire with _peer_store_release(). */
static inline FPR_STORE_HASH_TYPE *_peer_store_acquire(fpr_node_t *net, const uint8_t *mac)
{
    if (!mac) {
        return NULL;
    }
    _peers_lock(net);
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(net, mac);
    if (peer) {
        peer->refcount++;
    }
    _peers_unlock(net);
    return peer;
}

static inline void _peer_store_release(fpr_node_t *net, FPR_STORE_HASH_TYPE *peer)
{
    if (!peer) {
        return;
    }
    _peers_lock(net);
    bool free_now = (--peer->refcount == 0 && peer->dead);
    _peers_unlock(net);
    if (free_now) {
        if (peer->response_queue != NULL) {
            vQueueDelete(peer->response_queue);
        }
        fpr_security_clear_keys(&peer->security);
        heap_caps_free(peer);
    }
}

/* Clear per-session RX state: sequence watermark, fragment reassembly
 * and queued packets. A completed handshake starts a fresh session —
 * stale counters or half-read fragments from the old one must not leak
 * in. Call only under _peers_lock(). */
static inline void _peer_clear_session(FPR_STORE_HASH_TYPE *peer)
{
    if (!peer) {
        return;
    }
    peer->last_seq_num = 0;
    peer->receiving_fragmented = false;
    peer->fragment_seq_num = 0;
    if (peer->response_queue != NULL) {
        xQueueReset(peer->response_queue);
        peer->queued_packets = 0;
    }
}

/* Node-callback dispatch — a ctx-carrying node callback set via
 * fpr_node_config_t wins; the ctx-less config field is the fallback
 * so fpr_*_set_config()/fpr_set_callbacks() callers behave
 * identically. */
static inline void _cb_peer_discovered(fpr_node_t *net, const uint8_t *mac,
                                       const char *name, int8_t rssi)
{
    if (net->callbacks.on_peer_discovered) {
        net->callbacks.on_peer_discovered(net->callbacks.user, mac, name, rssi);
    } else if (net->client_config.discovery_cb) {
        net->client_config.discovery_cb(mac, name, rssi);
    }
}

static inline bool _cb_has_host_select(const fpr_node_t *net)
{
    return net->callbacks.on_host_selection || net->client_config.selection_cb;
}

static inline bool _cb_host_select(fpr_node_t *net, const uint8_t *mac,
                                   const char *name, int8_t rssi)
{
    if (net->callbacks.on_host_selection) {
        return net->callbacks.on_host_selection(net->callbacks.user, mac, name, rssi);
    }
    return net->client_config.selection_cb(mac, name, rssi);
}

static inline bool _cb_has_conn_request(const fpr_node_t *net)
{
    return net->callbacks.on_connection_request || net->host_config.request_cb;
}

static inline bool _cb_conn_request(fpr_node_t *net, const uint8_t *mac,
                                    const char *name, uint32_t key)
{
    if (net->callbacks.on_connection_request) {
        return net->callbacks.on_connection_request(net->callbacks.user, mac, name, key);
    }
    return net->host_config.request_cb(mac, name, key);
}

/* The node data callback gets a plain (mac, data, len) triple plus the
 * config-supplied user pointer. */
static inline void _cb_data(fpr_node_t *net, const uint8_t *peer_mac,
                            void *data, int len)
{
    if (net->callbacks.on_data) {
        net->callbacks.on_data(net->callbacks.user, peer_mac, data, (size_t)len);
    }
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

void _peer_store_data(fpr_node_t *net, const fpr_rx_frame_t *frame, const fpr_package_t *data);

esp_err_t _peer_add(fpr_node_t *net, const uint8_t *peer_mac, const char *name, bool is_connected);

esp_err_t _peer_add_discovered(fpr_node_t *net, const char *name, const uint8_t *address, bool is_connected);

/* Remove a peer from peers_map and the transport peer table. */
esp_err_t _peer_remove(fpr_node_t *net, const uint8_t *peer_mac);
/* hashmap_foreach callback freeing route-table entries. */
void _route_free_cb(void *key, void *value, void *user_data);

void _peer_copy_to_info(const FPR_STORE_HASH_TYPE *peer, fpr_peer_info_t *info);
/* Defined in fpr_send.c — shared by the handshake code. */
fpr_connect_t _make_fpr_info_with_keys(fpr_node_t *net, bool include_pwk, bool include_lwk, const uint8_t *pwk, const uint8_t *lwk);

/* hashmap_foreach payload for the peer-listing APIs: copy every store to
 * fpr_peer_info_t up to ctx capacity, optionally connected-only. Call
 * under _peers_lock() — peers_map iteration is not re-entrant. */
typedef struct {
    fpr_peer_info_t *array;
    size_t max_peers;
    size_t count;
    bool connected_only;
} fpr_peer_collect_ctx_t;

void _peer_collect_cb(void *key, void *value, void *user_data);
