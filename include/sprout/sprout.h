/**
 * @file fpr.h
 * @brief FPR (Fast Peer Router) Network Protocol API
 *
 * FPR is a lightweight, secure mesh networking library for ESP32 devices
 * that enables WiFi-like connectivity without requiring traditional WiFi
 * infrastructure. Built on top of ESP-NOW.
 *
 * Features:
 * - Broadcast-based discovery and unicast connection handshake
 * - Host/Client modes with auto/manual connection flows
 * - WiFi WPA2-style 4-way security handshake
 * - Packet fragmentation for large payloads
 * - Network statistics and diagnostics
 * - Protocol versioning for forward/backward compatibility
 * 
 * Modes:
 * - HOST: Central coordinator accepting client connections (Stable)
 * - CLIENT: Connect to hosts and communicate (Stable)
 * - EXTENDER: Mesh relay for multi-hop routing (Under Development)
 * - BROADCAST: Send to all devices in range
 *
 * Usage:
 * 1. Initialize WiFi (required for ESP-NOW)
 * 2. Call sprout_init() with device name
 * 3. Set mode with sprout_set_mode()
 * 4. Register callback with sprout_register_receive_callback()
 * 5. Start with sprout_start()
 * 6. Use sprout_send() or sprout_broadcast() to send data
 *
 * @author Alejandro Ramirez
 * @version 1.0.0
 * @date December 2025
 * @since August 2025
 */

#pragma once

#include "sprout/sprout_def.h"
#include "version_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "stdint.h"
#include "esp_err.h"

#define SPROUT_PACKAGE_INIT 0
#define SPROUT_PACKAGE_DATA 1

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Sprout network with device name.
 * @param name Device name.
 * @return ESP_OK on success, error code otherwise.
 * @note Uses default channel (current WiFi channel) and normal power mode.
 */
esp_err_t sprout_init(const char *name);

/**
 * @brief Deinitialize the Sprout network and release resources.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_deinit();

/**
 * @brief Start the Sprout network.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_start();

/**
 * @brief Stop the Sprout network.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_stop();

/**
 * @brief Pause the Sprout network without full stop.
 * @return ESP_OK on success, error code otherwise.
 * @note Paused network can be resumed with sprout_resume(). Connections are maintained.
 */
esp_err_t sprout_pause(void);

/**
 * @brief Resume the Sprout network after pause.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not paused.
 */
esp_err_t sprout_resume(void);

/**
 * @brief Get the current network state.
 * @return Network state (UNINITIALIZED, INITIALIZED, STARTED, PAUSED, STOPPED).
 */
sprout_network_state_t sprout_get_state(void);

// ========== Configuration Setters (Function-First) ==========

/**
 * @brief Set the WiFi channel.
 * @param channel WiFi channel (1-14, 0 = auto/current).
 * @note Must be set before sprout_start().
 */
void sprout_set_channel(uint8_t channel);

/**
 * @brief Set the power management mode.
 * @param mode Power mode (SPROUT_POWER_NORMAL or SPROUT_POWER_LOW).
 * @note LOW power mode increases polling/broadcast intervals to save battery.
 *       Can be changed at runtime.
 */
void sprout_set_power_mode(sprout_power_mode_t mode);

/**
 * @brief Get the current power management mode.
 * @return Current power mode.
 */
sprout_power_mode_t sprout_get_power_mode(void);

/**
 * @brief Get the configured WiFi channel.
 * @return WiFi channel (1-14), or 0 if using auto/current channel.
 */
uint8_t sprout_get_channel(void);

/**
 * @brief Set the default queue mode for new peers.
 * @param mode Queue mode (SPROUT_QUEUE_MODE_NORMAL or SPROUT_QUEUE_MODE_LATEST_ONLY).
 * @note LATEST_ONLY mode keeps only the most recent packet, discarding older
 *       ones. Useful for real-time data where only current state matters.
 *       Affects newly added peers. Use sprout_set_peer_queue_mode() to
 *       change mode for existing peers.
 */
void sprout_set_queue_mode(sprout_queue_mode_t mode);

/**
 * @brief Get the default queue mode.
 * @return Current default queue mode.
 */
sprout_queue_mode_t sprout_get_queue_mode(void);

/**
 * @brief Set queue mode for a specific peer.
 * @param peer_mac MAC address of the peer.
 * @param mode Queue mode to set.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if peer not found.
 */
esp_err_t sprout_set_peer_queue_mode(uint8_t *peer_mac, sprout_queue_mode_t mode);

/**
 * @brief Get the number of complete packets queued for a peer.
 * @param peer_mac MAC address of the peer.
 * @return Number of complete packets in queue, or 0 if peer not found.
 * @note Useful for monitoring queue depth and detecting backlog.
 */
uint32_t sprout_get_peer_queued_packets(uint8_t *peer_mac);

/**
 * @brief Set the network operating mode.
 * @param mode Operating mode (SPROUT_MODE_CLIENT, SPROUT_MODE_HOST, SPROUT_MODE_EXTENDER, or SPROUT_MODE_BROADCAST).
 * @note Must be called before sprout_start(). Changing mode while network is running requires stop/start cycle.
 */
void sprout_set_mode(sprout_mode_type_t mode);

/**
 * @brief Get the current network operating mode.
 * @return Current mode (SPROUT_MODE_CLIENT, SPROUT_MODE_HOST, SPROUT_MODE_EXTENDER, or SPROUT_MODE_BROADCAST).
 */
sprout_mode_type_t sprout_get_mode(void);

/**
 * @brief Set network visibility/permission state.
 * @param state Visibility state (SPROUT_VISIBILITY_PUBLIC or SPROUT_VISIBILITY_PRIVATE).
 * @note SPROUT_VISIBILITY_PRIVATE restricts which devices can discover or connect to this device.
 */
void sprout_set_permission_state(sprout_visibility_t state);

/**
 * @brief Get current network visibility/permission state.
 * @return Current visibility state (SPROUT_VISIBILITY_PUBLIC or SPROUT_VISIBILITY_PRIVATE).
 */
sprout_visibility_t sprout_get_permission_state(void);

/**
 * @brief Add a new peer to the network.
 * @param peer_mac MAC address of the new peer.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_add_peer(uint8_t *peer_mac);

/**
 * @brief Remove a peer from the network.
 * @param peer_mac MAC address of the peer to remove.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_remove_peer(uint8_t *peer_mac);

/**
 * @brief Start the network discovery/maintenance loop task.
 * @param duration How long the loop task should run (in FreeRTOS ticks). Use portMAX_DELAY for infinite.
 * @param force_restart If true, restarts the task even if already running.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already running and force_restart is false.
 * @note This task handles periodic discovery broadcasts and connection maintenance.
 */
esp_err_t sprout_start_loop_task(TickType_t duration, bool force_restart);

/**
 * @brief Stop the network discovery/maintenance loop task.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_stop_loop_task();

/**
 * @brief Check if the loop task is currently running.
 * @return true if loop task is active, false otherwise.
 */
bool sprout_is_loop_task_running();

/**
 * @brief Send data to a peer.
 * @param peer_address MAC address of the peer to send data to.
 * @param data Data buffer to send.
 * @param size Size of data buffer.
 * @param package_id Package ID for routing.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_send(uint8_t *peer_address, void *data, int size, sprout_package_id_t package_id);

/**
 * @brief Broadcast data to all peers.
 * @param data Data buffer to broadcast.
 * @param size Length of data buffer.
 * @param package_id Package ID for routing.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_broadcast(void *data, int size, sprout_package_id_t package_id);

/**
 * @brief Send device information (name, capabilities) to a specific peer.
 * @param peer_address MAC address of the peer to send device info to.
 * @return ESP_OK on success, error code otherwise.
 * @note Used during handshake and connection establishment.
 */
esp_err_t sprout_send_device_info(uint8_t *peer_address);

/**
 * @brief Broadcast device information to all reachable peers.
 * @return ESP_OK on success, error code otherwise.
 * @note Useful for announcing presence or capabilities to the network.
 */
esp_err_t sprout_broadcast_device_info();

/**
 * @brief Gets number of peers found.
 * @return Count of discovered peers.
 */
int sprout_get_peer_count();

/**
 * @brief Find a peer by name.
 * @param peer_name Name of the peer to find.
 * @param mac_out Buffer to store peer MAC address (6 bytes) if found.
 * @return ESP_OK if peer found, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t sprout_get_peer_by_name(const char *peer_name, uint8_t *mac_out);

/**
 * @brief Remove all peers from the network.
 * @return ESP_OK on success, error code otherwise.
 * @note This clears the entire peer table.
 */
esp_err_t sprout_clear_all_peers(void);

/**
 * @brief Check if a specific peer is currently reachable.
 * @param peer_mac MAC address of the peer to check.
 * @param timeout_ms Maximum time to wait for response (milliseconds).
 * @return true if peer responded, false if timeout or unreachable.
 * @note Sends a ping message and waits for acknowledgment.
 */
bool sprout_is_peer_reachable(uint8_t *peer_mac, uint32_t timeout_ms);

/**
 * @brief Iterate through all known peers and invoke callback for each.
 * @param callback Function to call for each peer with peer information.
 * @param user_data Arbitrary user data pointer passed to the callback.
 * @return Number of peers iterated.
 */
size_t sprout_get_peers(sprout_data_receive_cb_t callback, void *user_data);

/**
 * @brief Register callback for receiving application data.
 * @param callback Function to call when data is received (NULL to unregister).
 */
void sprout_register_receive_callback(sprout_data_receive_cb_t callback);

// ========== VERSION API ==========

/**
 * @brief Get the current FPR protocol version.
 * @return Protocol version as packed uint32 (major << 16 | minor << 8 | patch).
 */
code_version_t sprout_get_protocol_version(void);

/**
 * @brief Get protocol version as human-readable string.
 * @param buf Buffer to store version string (e.g., "1.0.0").
 * @param buf_size Size of the buffer.
 */
void sprout_get_protocol_version_string(char *buf, size_t buf_size);

/**
 * @brief Get network statistics.
 * @param stats Pointer to structure to fill with statistics.
 */
void sprout_get_network_stats(sprout_network_stats_t *stats);

/**
 * @brief Reset network statistics counters to zero.
 */
void sprout_reset_network_stats(void);

/**
 * @brief Get detailed information about a specific peer.
 * @param peer_mac MAC address of the peer.
 * @param info Pointer to structure to fill with peer info.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if peer not found.
 */
esp_err_t sprout_get_peer_info(uint8_t *peer_mac, sprout_peer_info_t *info);

/**
 * @brief List all discovered peers.
 * @param peer_array Array to fill with peer information.
 * @param max_peers Maximum number of peers to return.
 * @return Number of peers actually returned.
 */
size_t sprout_list_all_peers(sprout_peer_info_t *peer_array, size_t max_peers);

/**
 * @brief Remove stale routes from routing table.
 * @param timeout_ms Age in milliseconds after which routes are considered stale.
 * @return Number of routes removed.
 */
size_t sprout_cleanup_stale_routes(uint32_t timeout_ms);

/**
 * @brief Print routing table to log for debugging.
 */
void sprout_print_route_table(void);

// ========== HOST MODE API ==========

/**
 * @brief Set maximum number of peers for host mode.
 * @param max_peers Maximum peers allowed (0 = unlimited).
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_host_set_max_peers(uint8_t max_peers);

/**
 * @brief Set connection mode for host mode.
 * @param mode Connection mode (SPROUT_CONNECTION_AUTO or SPROUT_CONNECTION_MANUAL).
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_host_set_connection_mode(sprout_connection_mode_t mode);

/**
 * @brief Register callback for connection requests (host mode, manual mode).
 * @param cb Callback function to approve/reject connections (NULL for auto mode).
 */
void sprout_host_register_request_callback(sprout_connection_request_cb_t cb);

/**
 * @brief Get count of connected peers in host mode.
 * @return Number of connected peers.
 */
extern size_t sprout_host_get_connected_count(void);

/**
 * @brief Manually approve a pending connection request (host mode).
 * @param peer_mac MAC address of peer to approve.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_host_approve_peer(uint8_t *peer_mac);

/**
 * @brief Manually reject a connection request (host mode).
 * @param peer_mac MAC address of peer to reject.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_host_reject_peer(uint8_t *peer_mac);

/**
 * @brief Block a peer from connecting (host mode).
 * @param peer_mac MAC address of peer to block.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_host_block_peer(uint8_t *peer_mac);

/**
 * @brief Unblock a previously blocked peer (host mode).
 * @param peer_mac MAC address of peer to unblock.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_host_unblock_peer(uint8_t *peer_mac);

/**
 * @brief Add a MAC address to the allowlist (host mode).
 * @param peer_mac MAC address to allow.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_host_allow_peer(uint8_t *peer_mac);

/**
 * @brief Remove a MAC address from the allowlist (host mode).
 * @param peer_mac MAC address to remove from allowlist.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_host_disallow_peer(uint8_t *peer_mac);

/**
 * @brief Enable allowlist filtering (host mode).
 * When enabled, only allowlisted peers can connect.
 * @return ESP_OK on success.
 */
extern esp_err_t sprout_host_enable_allowlist(void);

/**
 * @brief Disable allowlist filtering (host mode).
 * When disabled, any peer can connect (subject to blocking).
 * @return ESP_OK on success.
 */
extern esp_err_t sprout_host_disable_allowlist(void);

/**
 * @brief Check if allowlist filtering is enabled.
 * @return true if allowlist is enabled, false otherwise.
 */
extern bool sprout_host_is_allowlist_enabled(void);

/**
 * @brief Disconnect a connected peer (host mode).
 * @param peer_mac MAC address of peer to disconnect.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_host_disconnect_peer(uint8_t *peer_mac);

// ========== CLIENT MODE API ==========

/**
 * @brief Set connection mode for client mode.
 * @param mode Connection mode (SPROUT_CONNECTION_AUTO or SPROUT_CONNECTION_MANUAL).
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t sprout_client_set_connection_mode(sprout_connection_mode_t mode);

/**
 * @brief Register callback for peer discovery (client mode).
 * @param cb Callback function called when peers are discovered.
 */
void sprout_client_register_discovery_callback(sprout_peer_discovered_cb_t cb);

/**
 * @brief Register callback for host selection (client mode, manual mode).
 * @param cb Callback function to approve host connection (NULL for auto mode).
 */
void sprout_client_register_selection_callback(sprout_host_selection_cb_t cb);

/**
 * @brief Check if client is connected to a host.
 * @return true if connected to at least one host.
 */
extern bool sprout_client_is_connected(void);

/**
 * @brief Get information about the connected host.
 * @param mac_out Buffer to store host MAC address (6 bytes).
 * @param name_out Buffer to store host name (optional, can be NULL).
 * @param name_size Size of name buffer.
 * @return ESP_OK if connected host found, ESP_ERR_NOT_FOUND otherwise.
 */
extern esp_err_t sprout_client_get_host_info(uint8_t *mac_out, char *name_out, size_t name_size);

/**
 * @brief List discovered hosts available for connection (client mode).
 * @param peer_array Array to fill with discovered host information.
 * @param max_peers Maximum number of hosts to return.
 * @return Number of discovered hosts actually returned.
 */
extern size_t sprout_client_list_discovered_hosts(sprout_peer_info_t *peer_array, size_t max_peers);

/**
 * @brief Manually connect to a specific discovered host (client mode).
 * @param peer_mac MAC address of host to connect to.
 * @param timeout Maximum time to wait for connection.
 * @return ESP_OK if connected, error code otherwise.
 */
extern esp_err_t sprout_client_connect_to_host(uint8_t *peer_mac, TickType_t timeout);

/**
 * @brief Disconnect from current host (client mode).
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t sprout_client_disconnect(void);

/**
 * @brief Start scanning for available hosts without connecting (client mode).
 * @param duration How long to scan for hosts.
 * @return Number of hosts discovered.
 */
extern size_t sprout_client_scan_for_hosts(TickType_t duration);

// ========== DATA RECEPTION API ==========

/**
 * @brief Wait for and retrieve data from a specific peer (blocking).
 * @param peer_mac MAC address of the peer to receive data from.
 * @param data Buffer to store received data.
 * @param data_size Size of the data buffer.
 * @param timeout Maximum time to wait for data (in FreeRTOS ticks).
 * @return true if data was received within timeout, false otherwise.
 * @note This is a blocking call. Use the callback API for non-blocking async reception.
 */
bool sprout_get_data_from_peer(uint8_t *peer_mac, void *data, int data_size, TickType_t timeout);

// ========== RECONNECT/KEEPALIVE API ==========

/**
 * @brief Start persistent background reconnect/keepalive monitoring.
 * Automatically monitors connection state and sends keepalives to maintain connections.
 * Works independently of discovery loops - keeps connections alive indefinitely.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already running.
 */
esp_err_t sprout_start_reconnect_task(void);

/**
 * @brief Stop persistent reconnect/keepalive monitoring task.
 * @return ESP_OK on success.
 */
esp_err_t sprout_stop_reconnect_task(void);

/**
 * @brief Check if reconnect task is running.
 * @return true if running, false otherwise.
 */
bool sprout_is_reconnect_task_running(void);

#ifdef __cplusplus
}
#endif