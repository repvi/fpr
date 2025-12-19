#include "fpr/fpr_extender.h"
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
//   fpr_set_extender_mode();  // Enable routing/forwarding
//

typedef struct {
    fpr_package_type_t package_type;
    fpr_package_id_t package_id;
    uint8_t max_hops;
} fpr_send_options_full_control_t;


static const char *TAG = "fpr_extender";

static bool _should_forward_packet(fpr_package_t *package, const uint8_t *src_mac)
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
    
    // Check if destination is broadcast or someone else
    const uint8_t broadcast_mac[6] = FPR_BROADCAST_ADDRESS;
    bool is_broadcast_dest = (memcmp(package->dest_mac, broadcast_mac, 6) == 0);
    bool is_for_me = (memcmp(package->dest_mac, fpr_net.mac, 6) == 0);
    
    // Forward if broadcast or not for me
    return (is_broadcast_dest || !is_for_me);
}

static esp_err_t fpr_send_data_full_control(uint8_t *peer_address, void *data, int size, const fpr_send_options_full_control_t *options)
{
    ESP_RETURN_ON_FALSE(data != NULL && size > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid data or size");
    ESP_RETURN_ON_FALSE(options != NULL, ESP_ERR_INVALID_ARG, TAG, "Options cannot be NULL");
    ESP_RETURN_ON_FALSE(size <= sizeof(((fpr_package_t*)0)->protocol), ESP_ERR_INVALID_SIZE, TAG, "Data too large for packet");
    
    fpr_package_t package = {0};
    package.package_type = options->package_type;
    package.id = options->package_id;
    memcpy(&package.protocol, data, size);
    
    // Initialize routing fields
    memcpy(package.origin_mac, fpr_net.mac, 6);
    if (peer_address) {
        memcpy(package.dest_mac, peer_address, 6);
    } else {
        memset(package.dest_mac, 0xFF, 6);
    }
    package.hop_count = 0;
    package.max_hops = options->max_hops > 0 ? options->max_hops : FPR_DEFAULT_MAX_HOPS;
    
    esp_err_t result = esp_now_send(peer_address, (const uint8_t *)&package, sizeof(package));
    if (result == ESP_OK) {
        fpr_net.stats.packets_sent++;
    } else {
        fpr_net.stats.send_failures++;
    }
    return result;
}

void _handle_extender_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    #if (FPR_DEBUG_LOG_EXTENDER_DATA_RECEIVE == 1)
    ESP_LOGI(TAG, "Extender received packet - len: %d, from: " MACSTR ", to: " MACSTR,
             len, MAC2STR(esp_now_info->src_addr), MAC2STR(esp_now_info->des_addr));
    #endif
    
    // Check if network is paused
    if (fpr_net.paused) {
        fpr_net.stats.packets_dropped++;
        return;  // Drop all packets when paused
    }
    
    if (!is_fpr_package_compatible(len)) {
        fpr_net.stats.packets_dropped++;
        return;
    }
    
    fpr_net.stats.packets_received++;
    fpr_package_t *package = (fpr_package_t *)data;
    
    // Version handling (using fpr_handle.h)
    if (!fpr_version_handle_version(esp_now_info, data, len, package->version)) {
        fpr_net.stats.packets_dropped++;
        return; // Version handler rejected the packet
    }
    
    // Update or add peer routing info
    FPR_STORE_HASH_TYPE *peer = _get_peer_from_map(esp_now_info->src_addr);
    if (peer) {
        _update_peer_rssi_and_timestamp(peer, esp_now_info);
        peer->packets_received++;
        
        // Update routing table if this is a better route
        if (package->hop_count + 1 < peer->hop_count || peer->hop_count == 0) {
            peer->hop_count = package->hop_count + 1;
            memcpy(peer->next_hop_mac, esp_now_info->src_addr, 6);
            ESP_LOGI(TAG, "Updated route to " MACSTR " via " MACSTR " (hops: %d)", 
                     MAC2STR(package->origin_mac), MAC2STR(esp_now_info->src_addr), peer->hop_count);
        }
    } else {
        // Add new peer to routing table
        _add_peer_internal(esp_now_info->src_addr, NULL, false, 0);
        peer = _get_peer_from_map(esp_now_info->src_addr);
        if (peer) {
            peer->hop_count = package->hop_count + 1;
            memcpy(peer->next_hop_mac, esp_now_info->src_addr, 6);
            _update_peer_rssi_and_timestamp(peer, esp_now_info);
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
            xQueueSend(peer->response_queue, data, 0);
        }
        ESP_LOGI(TAG, "Extender received packet from " MACSTR " (hops: %d)", 
                 MAC2STR(package->origin_mac), package->hop_count);
    }
    
    // Forward packet if routing enabled and appropriate
    if (fpr_net.routing_enabled && _should_forward_packet(package, esp_now_info->src_addr)) {
        package->hop_count++;  // Increment hop count
        
        // Determine next hop
        uint8_t *next_hop = NULL;
        if (is_broadcast) {
            // Broadcast to all except sender
            next_hop = (uint8_t *)broadcast_mac;
        } else {
            // Look up route to destination
            FPR_STORE_HASH_TYPE *dest_peer = _get_peer_from_map(package->dest_mac);
            if (dest_peer && dest_peer->hop_count > 0) {
                next_hop = dest_peer->next_hop_mac;
            }
        }
        
        if (next_hop) {
            fpr_send_options_full_control_t options = {
                .package_type = package->package_type,
                .package_id = package->id,
                .max_hops = package->max_hops
            };
            esp_err_t err = fpr_send_data_full_control(next_hop, (void *)&package->protocol, sizeof(package->protocol), &options);
            if (err == ESP_OK) {
                fpr_net.stats.packets_forwarded++;
                ESP_LOGD(TAG, "Forwarded packet from " MACSTR " to " MACSTR " (hop %d/%d)",
                         MAC2STR(package->origin_mac), MAC2STR(next_hop), 
                         package->hop_count, package->max_hops);
            } else {
                fpr_net.stats.send_failures++;
                ESP_LOGW(TAG, "Failed to forward packet: %s", esp_err_to_name(err));
            }
        } else {
            fpr_net.stats.packets_dropped++;
        }
    }
}