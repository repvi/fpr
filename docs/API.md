# FPR API Reference

Complete API documentation for the Fast Peer Router (FPR) library.

## Table of Contents

- [Network Lifecycle](#network-lifecycle)
- [Network State Management](#network-state-management)
- [Network Mode Management](#network-mode-management)
- [Discovery & Maintenance](#discovery--maintenance)
- [Data Transmission](#data-transmission)
- [Peer Management](#peer-management)
- [Client Mode API](#client-mode-api)
- [Host Mode API](#host-mode-api)
- [Network Information](#network-information)
- [Statistics & Diagnostics](#statistics--diagnostics)
- [Version Management](#version-management)
- [Data Types](#data-types)
- [Constants](#constants)

---

## Network Lifecycle

Core functions for initializing and managing the FPR network.

### `fpr_network_init()`

Initialize the FPR network with a device name.

```c
esp_err_t fpr_network_init(const char *name);
```

**Parameters:**
- `name` - Device name (max 31 characters)

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Must be called before any other FPR functions
- WiFi must be initialized before calling this function
- Device name is used for identification during discovery

**Example:**
```c
esp_err_t ret = fpr_network_init("MyDevice");
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize FPR: %s", esp_err_to_name(ret));
}
```

---

### `fpr_network_deinit()`

Deinitialize the FPR network and release all resources.

```c
esp_err_t fpr_network_deinit();
```

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Stops all running tasks
- Releases allocated memory
- Removes all peers
- Should be called before application exit

---

### `fpr_network_start()`

Start the FPR network operations.

```c
esp_err_t fpr_network_start();
```

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Mode must be set before starting (via `fpr_network_set_mode()`)
- Begins listening for ESP-NOW packets
- Initializes internal state machines

---

### `fpr_network_stop()`

Stop the FPR network operations.

```c
esp_err_t fpr_network_stop();
```

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Stops discovery and maintenance tasks
- Does not release resources (use `fpr_network_deinit()` for cleanup)
- Can be restarted with `fpr_network_start()`

---

## Network State Management

Functions for managing network state and controlling pause/resume functionality.

### `fpr_network_get_state()`

Get the current network state.

```c
fpr_network_state_t fpr_network_get_state(void);
```

**Returns:**
- Network state:
  - `FPR_STATE_UNINITIALIZED` - Not initialized
  - `FPR_STATE_INITIALIZED` - Initialized but not started
  - `FPR_STATE_STARTED` - Started and running
  - `FPR_STATE_PAUSED` - Paused (can be resumed)
  - `FPR_STATE_STOPPED` - Stopped (can be restarted)

**Example:**
```c
if (fpr_network_get_state() == FPR_STATE_STARTED) {
    printf("Network is running\n");
}
```

---

### `fpr_network_pause()`

Pause the FPR network without full stop.

```c
esp_err_t fpr_network_pause(void);
```

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not started

**Notes:**
- Paused network blocks all send operations
- Incoming packets are dropped
- Connections and peer state are maintained
- Can be resumed with `fpr_network_resume()`
- Useful for temporarily suspending network activity

**Example:**
```c
// Pause during critical operation
fpr_network_pause();
perform_critical_operation();
fpr_network_resume();
```

---

### `fpr_network_resume()`

Resume the FPR network after pause.

```c
esp_err_t fpr_network_resume(void);
```

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if not paused

**Notes:**
- Resumes normal network operations
- Send and receive operations are re-enabled

---

## Network Mode Management

Functions for setting and querying the network operating mode.

### `fpr_network_set_mode()`

Set the network operating mode.

```c
void fpr_network_set_mode(fpr_mode_type_t mode);
```

**Parameters:**
- `mode` - Operating mode:
  - `FPR_MODE_DEFAULT` - Default mode (no specific role)
  - `FPR_MODE_CLIENT` - Client mode (connects to hosts)
  - `FPR_MODE_HOST` - Host mode (accepts client connections)
  - `FPR_MODE_EXTENDER` - Extender mode (mesh relay)
  - `FPR_MODE_BROADCAST` - Broadcast mode (send-only)

**Notes:**
- Should be called before `fpr_network_start()`
- Changing mode while running requires stop/start cycle
- Host mode initializes security subsystem

**Example:**
```c
fpr_network_set_mode(FPR_MODE_HOST);
fpr_network_start();
```

---

### `fpr_network_get_mode()`

Get the current network operating mode.

```c
fpr_mode_type_t fpr_network_get_mode();
```

**Returns:**
- Current operating mode

---

## Discovery & Maintenance

Functions for managing network discovery and connection maintenance.

### `fpr_network_start_loop_task()`

Start the network discovery and maintenance loop task.

```c
esp_err_t fpr_network_start_loop_task(TickType_t duration, bool force_restart);
```

**Parameters:**
- `duration` - How long to run (in FreeRTOS ticks). Use `portMAX_DELAY` for infinite
- `force_restart` - If `true`, restarts task even if already running

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if already running and `force_restart` is `false`

**Notes:**
- Handles periodic discovery broadcasts
- Manages connection state
- Required for automatic peer discovery

**Example:**
```c
// Run discovery for 30 seconds
fpr_network_start_loop_task(pdMS_TO_TICKS(30000), false);

// Run discovery indefinitely
fpr_network_start_loop_task(portMAX_DELAY, false);
```

---

### `fpr_network_stop_loop_task()`

Stop the network discovery and maintenance loop task.

```c
esp_err_t fpr_network_stop_loop_task();
```

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

### `fpr_network_is_loop_task_running()`

Check if the loop task is currently running.

```c
bool fpr_network_is_loop_task_running();
```

**Returns:**
- `true` if loop task is active
- `false` otherwise

---

### `fpr_network_start_reconnect_task()`

Start persistent background reconnect and keepalive monitoring.

```c
esp_err_t fpr_network_start_reconnect_task(void);
```

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_INVALID_STATE` if already running

**Notes:**
- Automatically monitors connection state
- Sends periodic keepalive messages
- Works independently of discovery loop
- Keeps connections alive indefinitely

---

### `fpr_network_stop_reconnect_task()`

Stop the reconnect and keepalive monitoring task.

```c
esp_err_t fpr_network_stop_reconnect_task(void);
```

**Returns:**
- `ESP_OK` on success

---

### `fpr_network_is_reconnect_task_running()`

Check if the reconnect task is running.

```c
bool fpr_network_is_reconnect_task_running(void);
```

**Returns:**
- `true` if running
- `false` otherwise

---

## Data Transmission

Functions for sending data to peers.

### `fpr_network_send_to_peer()`

Send data to a specific peer.

```c
esp_err_t fpr_network_send_to_peer(uint8_t *peer_address, void *data, int size, fpr_package_id_t package_id);
```

**Parameters:**
- `peer_address` - MAC address of destination peer (6 bytes)
- `data` - Data buffer to send
- `size` - Size of data buffer (max 45 bytes for ESP-NOW)
- `package_id` - Application-defined package identifier

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Example:**
```c
uint8_t peer_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
char message[] = "Hello!";
esp_err_t ret = fpr_network_send_to_peer(peer_mac, message, strlen(message), 1);
```

---

### `fpr_network_broadcast()`

Broadcast data to all peers.

```c
esp_err_t fpr_network_broadcast(void *data, int size, fpr_package_id_t package_id);
```

**Parameters:**
- `data` - Data buffer to broadcast
- `size` - Size of data buffer
- `package_id` - Application-defined package identifier

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Example:**
```c
char announcement[] = "Server started";
fpr_network_broadcast(announcement, strlen(announcement), 1);
```

---

### `fpr_send_with_options()`

Send data with custom routing options.

```c
esp_err_t fpr_send_with_options(uint8_t *peer_address, void *data, int size, const fpr_send_options_t *options);
```

**Parameters:**
- `peer_address` - Destination MAC or `NULL` for broadcast
- `data` - Data buffer to send
- `size` - Size of data
- `options` - Send options structure:
  - `package_id` - Package identifier
  - `max_hops` - Maximum routing hops allowed

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Example:**
```c
fpr_send_options_t opts = {
    .package_id = 1,
    .max_hops = 3
};
fpr_send_with_options(peer_mac, data, size, &opts);
```

---

### `fpr_network_send_device_info()`

Send device information to a specific peer.

```c
esp_err_t fpr_network_send_device_info(uint8_t *peer_address);
```

**Parameters:**
- `peer_address` - MAC address of destination peer

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Used during handshake and connection establishment
- Sends device name, capabilities, and version

---

### `fpr_network_broadcast_device_info()`

Broadcast device information to all reachable peers.

```c
esp_err_t fpr_network_broadcast_device_info();
```

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Useful for announcing presence to the network
- Part of discovery protocol

---

### `fpr_register_receive_callback()`

Register a callback for receiving application data.

```c
void fpr_register_receive_callback(fpr_data_receive_cb_t callback);
```

**Parameters:**
- `callback` - Function to call when data is received (or `NULL` to unregister)

**Callback Signature:**
```c
typedef void(*fpr_data_receive_cb_t)(void *peer_addr, void *data, void *user_data);
```

**Example:**
```c
void on_data_received(void *peer_addr, void *data, void *user_data) {
    uint8_t *mac = (uint8_t*)peer_addr;
    printf("Data from %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (char*)data);
}

fpr_register_receive_callback(on_data_received);
```

---

### `fpr_network_get_data_from_peer()`

Wait for and retrieve data from a specific peer (blocking).

```c
bool fpr_network_get_data_from_peer(uint8_t *peer_mac, void *received_value, TickType_t timeout);
```

**Parameters:**
- `peer_mac` - MAC address of peer to receive from
- `received_value` - Buffer to store received data
- `timeout` - Maximum wait time (in FreeRTOS ticks)

**Returns:**
- `true` if data received within timeout
- `false` otherwise

**Notes:**
- This is a **blocking** call
- Use callback API (`fpr_register_receive_callback()`) for non-blocking reception

---

## Peer Management

Functions for managing peers in the network.

### `fpr_network_add_peer()`

Manually add a peer to the network.

```c
esp_err_t fpr_network_add_peer(uint8_t *peer_mac);
```

**Parameters:**
- `peer_mac` - MAC address of the peer (6 bytes)

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Usually not needed - discovery handles this automatically
- Useful for static network configurations

---

### `fpr_network_remove_peer()`

Remove a peer from the network.

```c
esp_err_t fpr_network_remove_peer(uint8_t *peer_mac);
```

**Parameters:**
- `peer_mac` - MAC address of the peer to remove

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

### `fpr_network_get_peer_count()`

Get the number of discovered peers.

```c
int fpr_network_get_peer_count();
```

**Returns:**
- Count of discovered peers

---

### `fpr_get_peer_by_name()`

Find a peer by name.

```c
esp_err_t fpr_get_peer_by_name(const char *peer_name, uint8_t *mac_out);
```

**Parameters:**
- `peer_name` - Name of the peer to find
- `mac_out` - Buffer to store peer MAC address (6 bytes) if found

**Returns:**
- `ESP_OK` if peer found
- `ESP_ERR_NOT_FOUND` if peer not found
- `ESP_ERR_INVALID_ARG` if parameters are NULL

**Example:**
```c
uint8_t peer_mac[6];
if (fpr_get_peer_by_name("Device-123", peer_mac) == ESP_OK) {
    fpr_network_send_to_peer(peer_mac, data, size, 1);
}
```

---

### `fpr_clear_all_peers()`

Remove all peers from the network.

```c
esp_err_t fpr_clear_all_peers(void);
```

**Returns:**
- `ESP_OK` on success

**Notes:**
- Clears the entire peer table
- Removes all peers from ESP-NOW
- Frees all peer resources

**Example:**
```c
// Reset network and clear all peers
fpr_clear_all_peers();
```

---

### `fpr_is_peer_reachable()`

Check if a specific peer is currently reachable.

```c
bool fpr_is_peer_reachable(uint8_t *peer_mac, uint32_t timeout_ms);
```

**Parameters:**
- `peer_mac` - MAC address of the peer to check
- `timeout_ms` - Maximum time to wait for response (milliseconds)

**Returns:**
- `true` if peer responded within timeout
- `false` if timeout or unreachable

**Notes:**
- Sends a ping message and waits for acknowledgment
- Checks if peer was recently seen or actively responds
- Blocking call - waits up to timeout_ms

**Example:**
```c
uint8_t peer_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
if (fpr_is_peer_reachable(peer_mac, 1000)) {
    printf("Peer is alive!\n");
} else {
    printf("Peer is unreachable\n");
}
```

---

### `fpr_network_get_peers()`

Iterate through all known peers with a callback.

```c
size_t fpr_network_get_peers(fpr_data_receive_cb_t callback, void *user_data);
```

**Parameters:**
- `callback` - Function to call for each peer
- `user_data` - Arbitrary user data passed to callback

**Returns:**
- Number of peers iterated

---

### `fpr_get_peer_info()`

Get detailed information about a specific peer.

```c
esp_err_t fpr_get_peer_info(uint8_t *peer_mac, fpr_peer_info_t *info);
```

**Parameters:**
- `peer_mac` - MAC address of the peer
- `info` - Pointer to structure to fill

**Returns:**
- `ESP_OK` on success
- `ESP_ERR_NOT_FOUND` if peer not found

**Returned Info Structure:**
```c
typedef struct {
    char name[PEER_NAME_MAX_LENGTH];
    uint8_t mac[MAC_ADDRESS_LENGTH];
    bool is_connected;
    fpr_peer_state_t state;
    uint8_t hop_count;
    int8_t rssi;
    uint64_t last_seen_ms;
    uint32_t packets_received;
} fpr_peer_info_t;
```

---

### `fpr_list_all_peers()`

List all discovered peers.

```c
size_t fpr_list_all_peers(fpr_peer_info_t *peer_array, size_t max_peers);
```

**Parameters:**
- `peer_array` - Array to fill with peer information
- `max_peers` - Maximum number of peers to return

**Returns:**
- Number of peers actually returned

**Example:**
```c
fpr_peer_info_t peers[20];
size_t count = fpr_list_all_peers(peers, 20);
for (size_t i = 0; i < count; i++) {
    printf("Peer %zu: %s (%02X:%02X:...)\n", i, peers[i].name, 
           peers[i].mac[0], peers[i].mac[1]);
}
```

---

### `fpr_network_set_permission_state()`

Set network visibility/permission state.

```c
void fpr_network_set_permission_state(fpr_visibility_t state);
```

**Parameters:**
- `state` - Visibility state:
  - `FPR_VISIBILITY_PUBLIC` - Discoverable by all devices
  - `FPR_VISIBILITY_PRIVATE` - Restricted discovery/connections

**Notes:**
- Private mode restricts which devices can discover or connect

---

### `fpr_network_get_permission_state()`

Get current network visibility/permission state.

```c
fpr_visibility_t fpr_network_get_permission_state(void);
```

**Returns:**
- Current visibility state

---

## Client Mode API

Functions specific to client mode operation.

### `fpr_client_set_config()`

Configure client mode behavior.

```c
esp_err_t fpr_client_set_config(const fpr_client_config_t *config);
```

**Parameters:**
- `config` - Client configuration structure:
  - `connection_mode` - `FPR_CONNECTION_AUTO` or `FPR_CONNECTION_MANUAL`
  - `discovery_cb` - Callback when hosts are discovered

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Example:**
```c
void on_host_discovered(const uint8_t *peer_mac, const char *peer_name, int8_t rssi) {
    printf("Found host: %s (RSSI: %d)\n", peer_name, rssi);
}

fpr_client_config_t config = {
    .connection_mode = FPR_CONNECTION_AUTO,
    .discovery_cb = on_host_discovered
};
fpr_client_set_config(&config);
```

---

### `fpr_client_get_config()`

Get current client configuration.

```c
esp_err_t fpr_client_get_config(fpr_client_config_t *config);
```

**Parameters:**
- `config` - Pointer to structure to fill

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

### `fpr_client_is_connected()`

Check if client is connected to a host.

```c
bool fpr_client_is_connected(void);
```

**Returns:**
- `true` if connected to at least one host
- `false` otherwise

---

### `fpr_client_get_host_info()`

Get information about the connected host.

```c
esp_err_t fpr_client_get_host_info(uint8_t *mac_out, char *name_out, size_t name_size);
```

**Parameters:**
- `mac_out` - Buffer to store host MAC (6 bytes)
- `name_out` - Buffer to store host name (or `NULL`)
- `name_size` - Size of name buffer

**Returns:**
- `ESP_OK` if connected
- `ESP_ERR_NOT_FOUND` if not connected

---

### `fpr_client_list_discovered_hosts()`

List all discovered hosts available for connection.

```c
size_t fpr_client_list_discovered_hosts(fpr_peer_info_t *peer_array, size_t max_peers);
```

**Parameters:**
- `peer_array` - Array to fill with host information
- `max_peers` - Maximum number of hosts to return

**Returns:**
- Number of hosts discovered

---

### `fpr_client_scan_for_hosts()`

Start scanning for available hosts without connecting.

```c
size_t fpr_client_scan_for_hosts(TickType_t duration);
```

**Parameters:**
- `duration` - How long to scan (in FreeRTOS ticks)

**Returns:**
- Number of hosts discovered

**Example:**
```c
// Scan for 5 seconds
size_t count = fpr_client_scan_for_hosts(pdMS_TO_TICKS(5000));
printf("Found %zu hosts\n", count);
```

---

### `fpr_client_connect_to_host()`

Manually connect to a specific discovered host.

```c
esp_err_t fpr_client_connect_to_host(uint8_t *peer_mac, TickType_t timeout);
```

**Parameters:**
- `peer_mac` - MAC address of host to connect to
- `timeout` - Maximum wait time for connection

**Returns:**
- `ESP_OK` if connected
- Error code on failure

**Notes:**
- Only works in manual connection mode

---

### `fpr_client_disconnect()`

Disconnect from the current host.

```c
esp_err_t fpr_client_disconnect(void);
```

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

## Host Mode API

Functions specific to host mode operation.

### `fpr_host_set_config()`

Configure host mode behavior.

```c
esp_err_t fpr_host_set_config(const fpr_host_config_t *config);
```

**Parameters:**
- `config` - Host configuration structure:
  - `max_peers` - Maximum peers allowed (0 = unlimited)
  - `connection_mode` - `FPR_CONNECTION_AUTO` or `FPR_CONNECTION_MANUAL`
  - `request_cb` - Callback for manual connection approval

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Example:**
```c
bool on_connection_request(const uint8_t *peer_mac, const char *peer_name, uint32_t peer_key) {
    printf("Connection request from: %s\n", peer_name);
    return true;  // Accept
}

fpr_host_config_t config = {
    .max_peers = 10,
    .connection_mode = FPR_CONNECTION_MANUAL,
    .request_cb = on_connection_request
};
fpr_host_set_config(&config);
```

---

### `fpr_host_get_config()`

Get current host configuration.

```c
esp_err_t fpr_host_get_config(fpr_host_config_t *config);
```

**Parameters:**
- `config` - Pointer to structure to fill

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

### `fpr_host_get_connected_count()`

Get count of connected peers.

```c
size_t fpr_host_get_connected_count(void);
```

**Returns:**
- Number of connected peers

---

### `fpr_host_approve_peer()`

Manually approve a pending connection request.

```c
esp_err_t fpr_host_approve_peer(uint8_t *peer_mac);
```

**Parameters:**
- `peer_mac` - MAC address of peer to approve

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Only works in manual connection mode

---

### `fpr_host_reject_peer()`

Manually reject a connection request.

```c
esp_err_t fpr_host_reject_peer(uint8_t *peer_mac);
```

**Parameters:**
- `peer_mac` - MAC address of peer to reject

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

### `fpr_host_block_peer()`

Block a peer from connecting.

```c
esp_err_t fpr_host_block_peer(uint8_t *peer_mac);
```

**Parameters:**
- `peer_mac` - MAC address of peer to block

**Returns:**
- `ESP_OK` on success
- Error code on failure

**Notes:**
- Blocked peers cannot connect until unblocked

---

### `fpr_host_unblock_peer()`

Unblock a previously blocked peer.

```c
esp_err_t fpr_host_unblock_peer(uint8_t *peer_mac);
```

**Parameters:**
- `peer_mac` - MAC address of peer to unblock

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

### `fpr_host_disconnect_peer()`

Disconnect a connected peer.

```c
esp_err_t fpr_host_disconnect_peer(uint8_t *peer_mac);
```

**Parameters:**
- `peer_mac` - MAC address of peer to disconnect

**Returns:**
- `ESP_OK` on success
- Error code on failure

---

## Network Information

Functions for querying network state and information.

### `fpr_cleanup_stale_routes()`

Remove stale routes from the routing table.

```c
size_t fpr_cleanup_stale_routes(uint32_t timeout_ms);
```

**Parameters:**
- `timeout_ms` - Age threshold (routes older than this are removed)

**Returns:**
- Number of routes removed

**Example:**
```c
// Remove routes older than 30 seconds
size_t removed = fpr_cleanup_stale_routes(30000);
printf("Removed %zu stale routes\n", removed);
```

---

### `fpr_print_route_table()`

Print the routing table to log for debugging.

```c
void fpr_print_route_table(void);
```

**Notes:**
- Output goes to ESP-IDF log system
- Useful for debugging mesh topology

---

## Statistics & Diagnostics

Functions for collecting network statistics and diagnostics.

### `fpr_get_network_stats()`

Get network statistics.

```c
void fpr_get_network_stats(fpr_network_stats_t *stats);
```

**Parameters:**
- `stats` - Pointer to structure to fill

**Statistics Structure:**
```c
typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_forwarded;
    uint32_t packets_dropped;
    uint32_t send_failures;
    size_t peer_count;
} fpr_network_stats_t;
```

**Example:**
```c
fpr_network_stats_t stats;
fpr_get_network_stats(&stats);
printf("Sent: %lu, Received: %lu, Dropped: %lu\n",
       stats.packets_sent, stats.packets_received, stats.packets_dropped);
```

---

### `fpr_reset_network_stats()`

Reset all network statistics counters to zero.

```c
void fpr_reset_network_stats(void);
```

---

## Version Management

Functions for protocol version management.

### `fpr_get_protocol_version()`

Get the current FPR protocol version.

```c
code_version_t fpr_get_protocol_version(void);
```

**Returns:**
- Protocol version as packed uint32: `(major << 16 | minor << 8 | patch)`

**Example:**
```c
code_version_t version = fpr_get_protocol_version();
uint8_t major = (version >> 16) & 0xFF;
uint8_t minor = (version >> 8) & 0xFF;
uint8_t patch = version & 0xFF;
printf("Protocol version: %d.%d.%d\n", major, minor, patch);
```

---

### `fpr_get_protocol_version_string()`

Get protocol version as human-readable string.

```c
void fpr_get_protocol_version_string(char *buf, size_t buf_size);
```

**Parameters:**
- `buf` - Buffer to store version string
- `buf_size` - Size of buffer

**Example:**
```c
char version_str[16];
fpr_get_protocol_version_string(version_str, sizeof(version_str));
printf("FPR Protocol: %s\n", version_str);  // Output: "FPR Protocol: 1.0.0"
```

---

## Data Types

### Enumerations

#### `fpr_mode_type_t`

Network operating modes.

```c
typedef enum {
    FPR_MODE_DEFAULT = 0,   // Default mode (no specific role)
    FPR_MODE_CLIENT,        // Client mode (connects to hosts)
    FPR_MODE_HOST,          // Host mode (accepts connections)
    FPR_MODE_BROADCAST,     // Broadcast mode (send-only)
    FPR_MODE_EXTENDER       // Extender/relay mode (mesh)
} fpr_mode_type_t;
```

---

#### `fpr_connection_mode_t`

Connection management modes.

```c
typedef enum {
    FPR_CONNECTION_AUTO = 0,    // Automatically accept/connect
    FPR_CONNECTION_MANUAL       // Require manual approval
} fpr_connection_mode_t;
```

---

#### `fpr_visibility_t`

Network visibility states.

```c
typedef enum {
    FPR_VISIBILITY_PUBLIC = 0,  // Discoverable by all
    FPR_VISIBILITY_PRIVATE      // Restricted discovery
} fpr_visibility_t;
```

---

#### `fpr_peer_state_t`

Peer connection states.

```c
typedef enum {
    FPR_PEER_STATE_DISCOVERED = 0,  // Found but not connected
    FPR_PEER_STATE_PENDING,         // Connection pending
    FPR_PEER_STATE_CONNECTED,       // Fully connected
    FPR_PEER_STATE_REJECTED,        // Connection rejected
    FPR_PEER_STATE_BLOCKED          // Blocked from connecting
} fpr_peer_state_t;
```

---

#### `fpr_network_state_t`

Network state enumeration.

```c
typedef enum {
    FPR_STATE_UNINITIALIZED = 0,  // Not initialized
    FPR_STATE_INITIALIZED,        // Initialized but not started
    FPR_STATE_STARTED,            // Started and running
    FPR_STATE_PAUSED,             // Paused (can be resumed)
    FPR_STATE_STOPPED             // Stopped (can be restarted)
} fpr_network_state_t;
```

---

### Structures

#### `fpr_peer_info_t`

Information about a peer.

```c
typedef struct {
    char name[PEER_NAME_MAX_LENGTH];     // Peer name
    uint8_t mac[MAC_ADDRESS_LENGTH];     // MAC address
    bool is_connected;                    // Connection status
    fpr_peer_state_t state;              // Peer state
    uint8_t hop_count;                   // Hops to reach peer
    int8_t rssi;                         // Signal strength
    uint64_t last_seen_ms;               // Last contact time
    uint32_t packets_received;           // Packet count
} fpr_peer_info_t;
```

---

#### `fpr_network_stats_t`

Network statistics.

```c
typedef struct {
    uint32_t packets_sent;         // Total packets sent
    uint32_t packets_received;     // Total packets received
    uint32_t packets_forwarded;    // Packets forwarded (extender mode)
    uint32_t packets_dropped;      // Dropped packets
    uint32_t send_failures;        // Failed send attempts
    size_t peer_count;             // Current peer count
} fpr_network_stats_t;
```

---

#### `fpr_send_options_t`

Send operation options.

```c
typedef struct {
    fpr_package_id_t package_id;   // Package identifier
    uint8_t max_hops;               // Maximum routing hops
} fpr_send_options_t;
```

---

#### `fpr_host_config_t`

Host mode configuration.

```c
typedef struct {
    uint8_t max_peers;                          // Max peers (0 = unlimited)
    fpr_connection_mode_t connection_mode;      // Auto/manual mode
    fpr_connection_request_cb_t request_cb;     // Approval callback
} fpr_host_config_t;
```

---

#### `fpr_client_config_t`

Client mode configuration.

```c
typedef struct {
    fpr_connection_mode_t connection_mode;      // Auto/manual mode
    fpr_peer_discovered_cb_t discovery_cb;      // Discovery callback
    fpr_host_selection_cb_t selection_cb;       // Host selection callback (manual mode)
} fpr_client_config_t;
```

**Fields:**
- `connection_mode`: Automatic or manual connection mode
- `discovery_cb`: Called when a host is discovered (manual mode)
- `selection_cb`: Called to select which discovered host to connect to (manual mode)

**Usage:**
```c
// Manual connection with host selection
fpr_client_config_t config = {
    .connection_mode = FPR_CONNECTION_MODE_MANUAL,
    .discovery_cb = on_host_discovered,
    .selection_cb = select_best_host
};
```

---

### Callback Types

#### `fpr_data_receive_cb_t`

Callback for receiving data.

```c
typedef void(*fpr_data_receive_cb_t)(void *peer_addr, void *data, void *user_data);
```

**Parameters:**
- `peer_addr` - MAC address of sender (6 bytes)
- `data` - Received data buffer
- `user_data` - User-defined context

---

#### `fpr_connection_request_cb_t`

Callback for connection requests (host mode).

```c
typedef bool(*fpr_connection_request_cb_t)(const uint8_t *peer_mac, 
                                           const char *peer_name, 
                                           uint32_t peer_key);
```

**Parameters:**
- `peer_mac` - Requesting peer's MAC
- `peer_name` - Requesting peer's name
- `peer_key` - Connection key

**Returns:**
- `true` to accept
- `false` to reject

---

#### `fpr_peer_discovered_cb_t`

Callback for peer discovery (client mode).

```c
typedef void(*fpr_peer_discovered_cb_t)(const uint8_t *peer_mac, 
                                        const char *peer_name, 
                                        int8_t rssi);
```

**Parameters:**
- `peer_mac` - Discovered peer's MAC
- `peer_name` - Discovered peer's name
- `rssi` - Signal strength

---

#### `fpr_host_selection_cb_t`

Callback for host selection (client manual mode).

```c
typedef bool(*fpr_host_selection_cb_t)(const uint8_t *peer_mac, 
                                       const char *peer_name, 
                                       int8_t rssi);
```

**Parameters:**
- `peer_mac` - Discovered host's MAC address
- `peer_name` - Discovered host's name
- `rssi` - Signal strength

**Returns:**
- `true` to connect to this host
- `false` to skip and continue discovery

**Usage:**
```c
bool select_best_host(const uint8_t *mac, const char *name, int8_t rssi) {
    // Connect to first host with strong signal
    if (rssi > -50) {
        printf("Connecting to %s (RSSI: %d)\n", name, rssi);
        return true;
    }
    return false;  // Keep searching
}

fpr_client_config_t config = {
    .connection_mode = FPR_CONNECTION_MODE_MANUAL,
    .discovery_cb = on_host_discovered,
    .selection_cb = select_best_host
};
```

---

## Constants

```c
#define MAC_ADDRESS_LENGTH 6           // MAC address size
#define PEER_NAME_MAX_LENGTH 32        // Maximum peer name length
#define FPR_PACKAGE_INIT 0            // Init package type
#define FPR_PACKAGE_DATA 1            // Data package type
```

---

## Complete Example

```c
#include "fpr/fpr.h"

void on_data_received(void *peer_addr, void *data, void *user_data) {
    uint8_t *mac = (uint8_t*)peer_addr;
    printf("Data from %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (char*)data);
}

void app_main() {
    // Initialize WiFi (not shown - see ESP-IDF examples)
    wifi_init();
    
    // Initialize FPR
    esp_err_t ret = fpr_network_init("MyDevice");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FPR init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Configure as host with auto-accept
    fpr_host_config_t host_config = {
        .max_peers = 10,
        .connection_mode = FPR_CONNECTION_AUTO,
        .request_cb = NULL
    };
    fpr_host_set_config(&host_config);
    
    // Set mode and start
    fpr_network_set_mode(FPR_MODE_HOST);
    fpr_register_receive_callback(on_data_received);
    fpr_network_start();
    
    // Start discovery
    fpr_network_start_loop_task(portMAX_DELAY, false);
    fpr_network_start_reconnect_task();
    
    // Send periodic broadcasts
    while (1) {
        char msg[] = "Hello from host!";
        fpr_network_broadcast(msg, strlen(msg), 1);
        
        // Print stats
        fpr_network_stats_t stats;
        fpr_get_network_stats(&stats);
        printf("Peers: %zu, Sent: %lu, Received: %lu\n",
               stats.peer_count, stats.packets_sent, stats.packets_received);
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

---

## See Also

- [README.md](../README.md) - Project overview
- [CONFIGURATION.md](CONFIGURATION.md) - Kconfig options
- [Test Suite](../test/README_TESTS.md) - Testing documentation
- [Quick Start](../test/QUICK_START.md) - Quick setup guide
