/**
 * @file fpr.c
 * @brief FPR (Fast Peer Router) network protocol over ESP-NOW
 *
 * Implements a small peer discovery, connection and optional forwarding
 * (extender/mesh) protocol that uses ESP-NOW for lightweight low-latency
 * device-to-device communication on ESP32 family devices.
 *
 * Features:
 * - Broadcast-based discovery and unicast connection handshake
 * - Optional client/host modes with auto/manual connection flows
 * - Simple mesh extender support (hop-count based forwarding)
 * - Small fixed-size packet format to fit ESP-NOW payload limits
 *
 * Usage notes:
 * - Call `fpr_network_init()` once, then `fpr_network_start()` to enable the
 *   protocol. Use `fpr_network_set_mode()` to switch between CLIENT/HOST/EXTENDER.
 * - Application data is delivered via a registered callback (`fpr_register_receive_callback`).
 * - The implementation expects callers to manage threading and to call
 *   this API from the main application/tasks (not from ISRs unless safe).
 *
 * Limitations and warnings:
 * - ESP-NOW payload size is limited; large payloads must be fragmented by the
 *   application if needed (not implemented here).
 * - This module uses heap allocations for peer state and queues; ensure
 *   sufficient heap and consider placing structures in DRAM/IRAM using
 *   `heap_caps` flags when required.
 * - The license and usage restrictions for the surrounding project are
 *   governed by the repository owner; see project documentation for details.
 *
 * @author Alejandro Ramirez
 * @date 2025-08-29
 * @since 2025-08-29
 */

#pragma once

#include "fpr/fpr_def.h"
#include "lib/version_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "stdint.h"
#include "esp_err.h"

#define FPR_PACKAGE_INIT 0
#define FPR_PACKAGE_DATA 1

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the FPR network with device name (default config).
 * @param name Device name.
 * @return ESP_OK on success, error code otherwise.
 * @note Uses default channel (current WiFi channel) and normal power mode.
 */
esp_err_t fpr_network_init(const char *name);

/**
 * @brief Initialize the FPR network with extended configuration.
 * @param name Device name.
 * @param config Initialization config (channel, power mode). Use FPR_INIT_CONFIG_DEFAULT() for defaults.
 * @return ESP_OK on success, error code otherwise.
 * @note Channel must be set before ESP-NOW init. WiFi must already be initialized.
 */
esp_err_t fpr_network_init_ex(const char *name, const fpr_init_config_t *config);

/**
 * @brief Deinitialize the FPR network and release resources.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_deinit();

/**
 * @brief Start the FPR network and register receive callback.
 * @param on_data_recv Callback for received data.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_start();

/**
 * @brief Stop the FPR network.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_stop();

/**
 * @brief Pause the FPR network without full stop.
 * @return ESP_OK on success, error code otherwise.
 * @note Paused network can be resumed with fpr_network_resume(). Connections are maintained.
 */
esp_err_t fpr_network_pause(void);

/**
 * @brief Resume the FPR network after pause.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not paused.
 */
esp_err_t fpr_network_resume(void);

/**
 * @brief Get the current network state.
 * @return Network state (UNINITIALIZED, INITIALIZED, STARTED, PAUSED, STOPPED).
 */
fpr_network_state_t fpr_network_get_state(void);

/**
 * @brief Set the power management mode.
 * @param mode Power mode (FPR_POWER_NORMAL or FPR_POWER_LOW).
 * @note LOW power mode increases polling/broadcast intervals to save battery.
 *       Can be changed at runtime.
 */
void fpr_network_set_power_mode(fpr_power_mode_t mode);

/**
 * @brief Get the current power management mode.
 * @return Current power mode.
 */
fpr_power_mode_t fpr_network_get_power_mode(void);

/**
 * @brief Get the configured WiFi channel.
 * @return WiFi channel (1-14), or 0 if using auto/current channel.
 */
uint8_t fpr_network_get_channel(void);

/**
 * @brief Set the network operating mode.
 * @param mode Operating mode (FPR_MODE_CLIENT, FPR_MODE_HOST, FPR_MODE_EXTENDER, or FPR_MODE_BROADCAST).
 * @note Must be called before fpr_network_start(). Changing mode while network is running requires stop/start cycle.
 */
void fpr_network_set_mode(fpr_mode_type_t mode);

/**
 * @brief Get the current network operating mode.
 * @return Current mode (FPR_MODE_CLIENT, FPR_MODE_HOST, FPR_MODE_EXTENDER, or FPR_MODE_BROADCAST).
 */
fpr_mode_type_t fpr_network_get_mode();

/**
 * @brief Add a new peer to the network.
 * @param peer_mac MAC address of the new peer.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_add_peer(uint8_t *peer_mac);

/**
 * @brief Remove a peer from the network.
 * @param peer_mac MAC address of the peer to remove.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_remove_peer(uint8_t *peer_mac);

/**
 * @brief Start the network discovery/maintenance loop task.
 * @param duration How long the loop task should run (in FreeRTOS ticks). Use portMAX_DELAY for infinite.
 * @param force_restart If true, restarts the task even if already running.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already running and force_restart is false.
 * @note This task handles periodic discovery broadcasts and connection maintenance.
 */
esp_err_t fpr_network_start_loop_task(TickType_t duration, bool force_restart);

/**
 * @brief Stop the network discovery/maintenance loop task.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_stop_loop_task();

/**
 * @brief Check if the loop task is currently running.
 * @return true if loop task is active, false otherwise.
 */
bool fpr_network_is_loop_task_running();

/**
 * @brief Send data with custom options (max hops, etc).
 * @param peer_address Destination MAC or NULL for broadcast.
 * @param data Data buffer to send.
 * @param size Size of data.
 * @param options Send options (max_hops, package_type, etc).
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_send_with_options(uint8_t *peer_address, void *data, int size, const fpr_send_options_t *options);

/**
 * @brief Send data to the connected peer.
 * @param peer_address MAC address of the peer to send data to.
 * @param data Data buffer to send.
 * @param size Size of data buffer.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_send_to_peer(uint8_t *peer_address, void *data, int size, fpr_package_id_t package_id);

/**
 * @brief Broadcast data to all peers.
 * @param data Data buffer to broadcast.
 * @param len Length of data buffer.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fpr_network_broadcast(void *data, int size, fpr_package_id_t package_id);

/**
 * @brief Send device information (name, capabilities) to a specific peer.
 * @param peer_address MAC address of the peer to send device info to.
 * @return ESP_OK on success, error code otherwise.
 * @note Used during handshake and connection establishment.
 */
esp_err_t fpr_network_send_device_info(uint8_t *peer_address);

/**
 * @brief Broadcast device information to all reachable peers.
 * @return ESP_OK on success, error code otherwise.
 * @note Useful for announcing presence or capabilities to the network.
 */
esp_err_t fpr_network_broadcast_device_info();

/**
 * @brief Gets number of peers found
 * @return Count of discovered peers
 */
int fpr_network_get_peer_count();

/**
 * @brief Find a peer by name.
 * @param peer_name Name of the peer to find.
 * @param mac_out Buffer to store peer MAC address (6 bytes) if found.
 * @return ESP_OK if peer found, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t fpr_get_peer_by_name(const char *peer_name, uint8_t *mac_out);

/**
 * @brief Remove all peers from the network.
 * @return ESP_OK on success, error code otherwise.
 * @note This clears the entire peer table.
 */
esp_err_t fpr_clear_all_peers(void);

/**
 * @brief Check if a specific peer is currently reachable.
 * @param peer_mac MAC address of the peer to check.
 * @param timeout_ms Maximum time to wait for response (milliseconds).
 * @return true if peer responded, false if timeout or unreachable.
 * @note Sends a ping message and waits for acknowledgment.
 */
bool fpr_is_peer_reachable(uint8_t *peer_mac, uint32_t timeout_ms);

/**
 * @brief Check if client is connected to a host.
 * @return true if connected to at least one host.
 */
extern bool fpr_client_is_connected(void);

/**
 * @brief Get information about the connected host.
 * @param mac_out Buffer to store host MAC address (6 bytes).
 * @param name_out Buffer to store host name (optional, can be NULL).
 * @param name_size Size of name buffer.
 * @return ESP_OK if connected host found, ESP_ERR_NOT_FOUND otherwise.
 */
extern esp_err_t fpr_client_get_host_info(uint8_t *mac_out, char *name_out, size_t name_size);

/**
 * @brief Set network visibility/permission state.
 * @param state Visibility state (FPR_VISIBILITY_PUBLIC or FPR_VISIBILITY_PRIVATE).
 * @note FPR_VISIBILITY_PRIVATE restricts which devices can discover or connect to this device.
 */
void fpr_network_set_permission_state(fpr_visibility_t state);

/**
 * @brief Get current network visibility/permission state.
 * @return Current visibility state (FPR_VISIBILITY_PUBLIC or FPR_VISIBILITY_PRIVATE).
 */
fpr_visibility_t fpr_network_get_permission_state(void);

/**
 * @brief Iterate through all known peers and invoke callback for each.
 * @param callback Function to call for each peer with peer information.
 * @param user_data Arbitrary user data pointer passed to the callback.
 * @return Number of peers iterated.
 */
size_t fpr_network_get_peers(fpr_data_receive_cb_t callback, void *user_data);

/**
 * @brief Register callback for receiving application data.
 * @param callback Function to call when data is received (NULL to unregister).
 */
void fpr_register_receive_callback(fpr_data_receive_cb_t callback);

// ========== VERSION API ==========

/**
 * @brief Get the current FPR protocol version.
 * @return Protocol version as packed uint32 (major << 16 | minor << 8 | patch).
 */
code_version_t fpr_get_protocol_version(void);

/**
 * @brief Get protocol version as human-readable string.
 * @param buf Buffer to store version string (e.g., "1.0.0").
 * @param buf_size Size of the buffer.
 */
void fpr_get_protocol_version_string(char *buf, size_t buf_size);

/**
 * @brief Get network statistics.
 * @param stats Pointer to structure to fill with statistics.
 */
void fpr_get_network_stats(fpr_network_stats_t *stats);

/**
 * @brief Reset network statistics counters to zero.
 */
void fpr_reset_network_stats(void);

/**
 * @brief Get detailed information about a specific peer.
 * @param peer_mac MAC address of the peer.
 * @param info Pointer to structure to fill with peer info.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if peer not found.
 */
esp_err_t fpr_get_peer_info(uint8_t *peer_mac, fpr_peer_info_t *info);

/**
 * @brief List all discovered peers.
 * @param peer_array Array to fill with peer information.
 * @param max_peers Maximum number of peers to return.
 * @return Number of peers actually returned.
 */
size_t fpr_list_all_peers(fpr_peer_info_t *peer_array, size_t max_peers);

/**
 * @brief Remove stale routes from routing table.
 * @param timeout_ms Age in milliseconds after which routes are considered stale.
 * @return Number of routes removed.
 */
size_t fpr_cleanup_stale_routes(uint32_t timeout_ms);

/**
 * @brief Print routing table to log for debugging.
 */
void fpr_print_route_table(void);

// ========== CONNECTION CONTROL API ==========

/**
 * @brief Configure host mode with connection control settings.
 * @param config Host configuration (max peers, auto/manual mode, callback).
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_host_set_config(const fpr_host_config_t *config);

/**
 * @brief Get current host configuration.
 * @param config Pointer to structure to fill with current config.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_host_get_config(fpr_host_config_t *config);

/**
 * @brief Get count of connected peers in host mode.
 * @return Number of connected peers.
 */
extern size_t fpr_host_get_connected_count(void);

/**
 * @brief Manually approve a pending connection request (host mode).
 * @param peer_mac MAC address of peer to approve.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_host_approve_peer(uint8_t *peer_mac);

/**
 * @brief Manually reject a connection request (host mode).
 * @param peer_mac MAC address of peer to reject.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_host_reject_peer(uint8_t *peer_mac);

/**
 * @brief Block a peer from connecting (host mode).
 * @param peer_mac MAC address of peer to block.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_host_block_peer(uint8_t *peer_mac);

/**
 * @brief Unblock a previously blocked peer (host mode).
 * @param peer_mac MAC address of peer to unblock.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_host_unblock_peer(uint8_t *peer_mac);

/**
 * @brief Disconnect a connected peer (host mode).
 * @param peer_mac MAC address of peer to disconnect.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_host_disconnect_peer(uint8_t *peer_mac);

/**
 * @brief Configure client mode with connection control settings.
 * @param config Client configuration (auto/manual mode, discovery callback).
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_client_set_config(const fpr_client_config_t *config);

/**
 * @brief Get current client configuration.
 * @param config Pointer to structure to fill with current config.
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_client_get_config(fpr_client_config_t *config);

/**
 * @brief List discovered hosts available for connection (client mode).
 * @param peer_array Array to fill with discovered host information.
 * @param max_peers Maximum number of hosts to return.
 * @return Number of discovered hosts actually returned.
 */
extern size_t fpr_client_list_discovered_hosts(fpr_peer_info_t *peer_array, size_t max_peers);

/**
 * @brief Manually connect to a specific discovered host (client mode).
 * @param peer_mac MAC address of host to connect to.
 * @param timeout Maximum time to wait for connection.
 * @return ESP_OK if connected, error code otherwise.
 */
extern esp_err_t fpr_client_connect_to_host(uint8_t *peer_mac, TickType_t timeout);

/**
 * @brief Disconnect from current host (client mode).
 * @return ESP_OK on success, error code otherwise.
 */
extern esp_err_t fpr_client_disconnect(void);

/**
 * @brief Start scanning for available hosts without connecting (client mode).
 * @param duration How long to scan for hosts.
 * @return Number of hosts discovered.
 */
extern size_t fpr_client_scan_for_hosts(TickType_t duration);

/**
 * @brief Wait for and retrieve data from a specific peer (blocking).
 * @param peer_mac MAC address of the peer to receive data from.
 * @param data Buffer to store received data.
 * @param data_size Size of the data buffer.
 * @param timeout Maximum time to wait for data (in FreeRTOS ticks).
 * @return true if data was received within timeout, false otherwise.
 * @note This is a blocking call. Use the callback API for non-blocking async reception.
 */
bool fpr_network_get_data_from_peer(uint8_t *peer_mac, void *data, int data_size, TickType_t timeout);

/**
 * @brief Start persistent background reconnect/keepalive monitoring.
 * Automatically monitors connection state and sends keepalives to maintain connections.
 * Works independently of discovery loops - keeps connections alive indefinitely.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already running.
 */
esp_err_t fpr_network_start_reconnect_task(void);

/**
 * @brief Stop persistent reconnect/keepalive monitoring task.
 * @return ESP_OK on success.
 */
esp_err_t fpr_network_stop_reconnect_task(void);

/**
 * @brief Check if reconnect task is running.
 * @return true if running, false otherwise.
 */
bool fpr_network_is_reconnect_task_running(void);

#ifdef __cplusplus
}
#endif