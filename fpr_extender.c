/**
 * @file fpr_extender.c
 * @brief FPR Extender/Mesh Mode Implementation
 * 
 * @warning UNDER DEVELOPMENT - This module is not yet production-ready.
 *          Extender mode will be completed in version 1.1.0.
 * 
 * Implements mesh networking functionality for multi-hop packet forwarding.
 * 
 * @version 1.0.0 (Under Development)
 * @date December 2025
 */

#include "fpr/fpr.h"
#include "fpr/internal/rx.h"
#include "fpr/internal/codec.h"
#include "esp_log.h"
#include "esp_check.h"

// ========== EXTENDER MODE HANDLERS ==========
//
// Extender mode enables mesh routing/forwarding capability.
// Devices in extender mode act as relays to extend network range.
//
// HOW IT WORKS:
// - Each packet contains: origin_mac, dest_mac, hop_count, max_hops
// - Extenders learn routes by tracking which neighbor to use for each destination
// - Packets are forwarded if: not from self, under hop limit, and not for self
// - Routing table updates when better routes discovered (lower hop count)
//
// NETWORK TOPOLOGY EXAMPLE:
//   [Client A] <---> [Extender 1] <---> [Extender 2] <---> [Host B]
//   
//   Client A can reach Host B even though they're out of direct range.
//   Packets hop through Extender 1 and 2 with increasing hop_count.
//
// USAGE:
//   fpr_network_init("Extender1");
//   fpr_network_start();
//   fpr_network_set_mode(FPR_MODE_EXTENDER);  // Enable routing/forwarding
//

static const char *TAG = "fpr_extender";

static bool _should_forward_packet(const fpr_package_t *package, bool is_for_me, bool is_broadcast)
{
    // Don't forward if we're the origin
    if (memcmp(package->origin_mac, fpr_net.mac, 6) == 0) {
        return false;
    }

    // Check TTL (hop count limit)
    if (package->hop_count >= package->max_hops) {
        ESP_LOGW(TAG, "Packet exceeded max hops (%d), dropping", package->max_hops);
        return false;
    }

    // Forward if broadcast or not for me
    return (is_broadcast || !is_for_me);
}

/* Forward the received package itself: origin/dest/sequence/version and
 * payload_size all survive, only hop_count advances. Rebuilding the
 * packet used to reset hop_count to 0 (TTL never expired), rewrite
 * origin to the extender and dest to the next hop — breaking route
 * learning, replay protection and TTL termination downstream. */
static esp_err_t _forward_package(const uint8_t *next_hop, const fpr_package_t *package)
{
    uint8_t fwd_buf[sizeof(*package)];
    size_t fwd_len = fpr_codec_encode(package, fwd_buf, sizeof(fwd_buf));
    const fpr_transport_t *t = _transport();
    esp_err_t result = fwd_len == 0 ? ESP_ERR_INVALID_SIZE
                       : t == NULL ? ESP_ERR_INVALID_STATE
                       : t->send(t->ctx, next_hop, fwd_buf, fwd_len);
    /* _rx_begin left the lock held; _stats_count_send re-takes it
     * recursively — no implicit "callers must hold" coupling. */
    _stats_count_send(result);
    return result;
}

void _rx_extender(const fpr_rx_frame_t *frame, const uint8_t *data, int len)
{
    #if (FPR_DEBUG_LOG_EXTENDER_DATA_RECEIVE == 1)
    ESP_LOGI(TAG, "Extender received packet - len: %d, from: " MACSTR ", to: " MACSTR,
             len, MAC2STR(frame->src_mac), MAC2STR(frame->dst_mac));
    #endif
    
    /* paused/size/version gates, control sanitize, peers_lock + quiesce
     * re-check — shared preamble, returns with the lock held. Extender
     * accounting (count_drops) charges rejects to packets_dropped and
     * counts size-valid packets in packets_received. */
    fpr_rx_ctx_t ctx;
    if (!_rx_begin(frame, data, len, true, &ctx)) {
        return;
    }
    /* Forwarding adjusts hop metadata, not the driver-owned RX buffer. */
    fpr_package_t received;
    memcpy(&received, data, sizeof(received));
    fpr_package_t *package = &received;

    // Update or add peer liveness info — serialize against remove/clear on
    // other tasks (peer stores are heap-owned by the map). The sender is a
    // direct neighbor: its distance is always 1 hop.
    FPR_STORE_HASH_TYPE *peer = _peer_from_map(frame->src_mac);
    if (peer) {
        _peer_touch(peer, frame);
        peer->packets_received++;
        if (peer->hop_count == 0) {
            peer->hop_count = 1;
        }
    } else {
        // Add new peer
        _peer_add(frame->src_mac, NULL, false);
        peer = _peer_from_map(frame->src_mac);
        if (peer) {
            peer->hop_count = 1;
            _peer_touch(peer, frame);
        }
    }

    /* Learn the route to the ORIGINATOR (not the sender): route[origin]
     * = {next_hop: sender, hops: origin_hop + 1}. Replace only on a
     * strictly better hop count. Self-originated packets teach nothing. */
    if (memcmp(package->origin_mac, fpr_net.mac, 6) != 0) {
        fpr_route_entry_t *route = hashmap_get(&fpr_net.routes_map, package->origin_mac);
        if (!route) {
            route = heap_caps_calloc(1, sizeof(*route), MALLOC_CAP_DEFAULT);
            if (route) {
                memcpy(route->dest_mac, package->origin_mac, 6);
                if (!hashmap_put(&fpr_net.routes_map, route->dest_mac, route)) {
                    heap_caps_free(route);
                    route = NULL;
                }
            }
        }
        if (route) {
            uint8_t via_hops = package->hop_count + 1;
            if (route->hop_count == 0 || via_hops < route->hop_count) {
                route->hop_count = via_hops;
                memcpy(route->next_hop_mac, frame->src_mac, 6);
                ESP_LOGI(TAG, "Learned route to " MACSTR " via " MACSTR " (hops: %d)",
                         MAC2STR(package->origin_mac), MAC2STR(frame->src_mac),
                         route->hop_count);
            }
            route->last_seen = esp_timer_get_time();
        }
    }
    
    // Check if this packet is for us
    bool is_for_me = (memcmp(package->dest_mac, fpr_net.mac, 6) == 0);
    const uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    bool is_broadcast = (memcmp(package->dest_mac, broadcast_mac, 6) == 0);
    
    if (is_for_me || is_broadcast) {
        // Process locally (store in queue if peer exists)
        if (peer && peer->response_queue) {
            // Non-blocking enqueue to avoid delaying RX path
            if (xQueueSend(peer->response_queue, data, 0) == pdPASS &&
                (package->package_type == FPR_PACKAGE_TYPE_SINGLE ||
                 package->package_type == FPR_PACKAGE_TYPE_END)) {
                peer->queued_packets++;
            }
        }
        ESP_LOGI(TAG, "Extender received packet from " MACSTR " (hops: %d)", 
                 MAC2STR(package->origin_mac), package->hop_count);
    }
    
    // Forward packet if routing enabled and appropriate
    if (fpr_net.routing_enabled && _should_forward_packet(package, is_for_me, is_broadcast)) {
        package->hop_count++;  // Increment hop count on our local copy

        // Determine next hop
        const uint8_t *next_hop = NULL;
        if (is_broadcast) {
            // Flood: downstream nodes re-derive broadcast from dest_mac
            next_hop = broadcast_mac;
        } else {
            // Look up the learned route to the final destination
            const fpr_route_entry_t *route =
                hashmap_get(&fpr_net.routes_map, package->dest_mac);
            if (route) {
                next_hop = route->next_hop_mac;
            } else {
                // Fallback: destination heard directly before routes existed
                FPR_STORE_HASH_TYPE *dest_peer = _peer_from_map(package->dest_mac);
                if (dest_peer && dest_peer->hop_count > 0) {
                    next_hop = dest_peer->next_hop_mac;
                    if (memcmp(next_hop, (const uint8_t[6]){0}, 6) == 0) {
                        next_hop = package->dest_mac;
                    }
                }
            }
        }

        if (next_hop) {
            esp_err_t err = _forward_package(next_hop, package);
            if (err == ESP_OK) {
                fpr_net.stats.packets_forwarded++;
                ESP_LOGD(TAG, "Forwarded packet from " MACSTR " to " MACSTR " (hop %d/%d)",
                         MAC2STR(package->origin_mac), MAC2STR(next_hop),
                         package->hop_count, package->max_hops);
            } else {
                ESP_LOGW(TAG, "Failed to forward packet: %s", esp_err_to_name(err));
            }
        } else {
            fpr_net.stats.packets_dropped++;
        }
    }
    _peers_unlock();
}