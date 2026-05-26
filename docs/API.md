# SPROUT Library Overview

> **Version 1.0.0 LTS** | December 2025

SPROUT (Simple Peer-to-Peer Routing Over Unified Transport) is a peer-to-peer networking library for ESP32 devices using ESP-NOW.

## What SPROUT Does

SPROUT enables ESP32 devices to communicate directly with each other without requiring a WiFi access point or router. Devices can discover each other, establish connections, and exchange data in various network topologies.

## Key Features

- **Direct Device Communication**: No WiFi AP or router needed
- **Automatic Discovery**: Devices find each other automatically
- **Multiple Network Modes**: Host, Client, Extender, and Broadcast
- **Reliable Data Transfer**: Built-in reliability and error handling
- **Mesh Routing**: Multi-hop message relay through extenders
- **Connection Management**: Automatic reconnection and peer tracking
- **Low Overhead**: Minimal memory and CPU usage

## Network Modes

### Host Mode
A host accepts connections from multiple clients. Hosts broadcast their presence and wait for clients to connect. Suitable for central controllers or gateways.

### Client Mode
A client actively searches for hosts and connects to them. Clients can send data to hosts and receive responses. Suitable for sensors, actuators, or edge devices.

### Extender Mode
An extender acts as a relay in a mesh network, forwarding messages between devices that are out of direct range. Extenders don't initiate connections but help extend network coverage.

### Broadcast Mode
Broadcast mode is for send-only scenarios where devices send messages without expecting responses. Useful for telemetry or status updates.

## Typical Workflow

1. **Initialize**: Set up the library with a device name
2. **Configure Mode**: Choose Host, Client, Extender, or Broadcast mode
3. **Register Callbacks**: Set up handlers for incoming data and events
4. **Start Network**: Begin discovery and connection processes
5. **Exchange Data**: Send and receive messages between devices
6. **Cleanup**: Stop and deinitialize when done

## Data Flow

Devices communicate using ESP-NOW, a connectionless WiFi protocol. SPROUT adds:
- Device discovery and identification
- Connection state management
- Message routing and relay
- Automatic reconnection
- Data validation

## Reliability Features

- **Keepalive Messages**: Periodic messages to detect disconnections
- **Automatic Reconnection**: Clients automatically reconnect to hosts
- **Peer State Tracking**: Monitors connection status of all peers
- **Message Acknowledgment**: Optional confirmation of message delivery
- **Error Recovery**: Handles packet loss and transmission errors

## Use Cases

- **Home Automation**: Sensors communicating with a central hub
- **Industrial Monitoring**: Multiple sensors reporting to a gateway
- **Remote Control**: Controllers sending commands to actuators
- **Telemetry Collection**: Devices sending status updates
- **Mesh Networks**: Extending range through relay devices

## Performance Characteristics

- **Range**: Up to 200m outdoors (line of sight)
- **Latency**: < 50ms for direct connections
- **Throughput**: Up to 1 Mbps (application dependent)
- **Memory**: ~50KB RAM for typical usage
- **Power**: Low power consumption with sleep support

## Limitations

- Requires ESP32 or ESP32-S2/S3 chips
- Maximum 250 bytes per message (ESP-NOW limitation)
- Best performance with 10-20 peers per device
- Requires WiFi to be initialized first
- Not compatible with standard WiFi stations/soft-AP simultaneously

## Getting Started

See the [README](../README.md) for basic usage examples and setup instructions.
**
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

sprout_client_config_t config = {
    .connection_mode = SPROUT_CONNECTION_MODE_MANUAL,
    .discovery_cb = on_host_discovered,
    .selection_cb = select_best_host
};
```

---

## Constants

```c
#define MAC_ADDRESS_LENGTH 6           // MAC address size
#define PEER_NAME_MAX_LENGTH 32        // Maximum peer name length
#define SPROUT_PACKAGE_INIT 0            // Init package type
#define SPROUT_PACKAGE_DATA 1            // Data package type
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
    esp_err_t ret = sprout_init("MyDevice");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FPR init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Configure as host with auto-accept
    sprout_host_config_t host_config = {
        .max_peers = 10,
        .connection_mode = SPROUT_CONNECTION_AUTO,
        .request_cb = NULL
    };
    sprout_host_set_config(&host_config);
    
    // Set mode and start
    sprout_set_mode(SPROUT_MODE_HOST);
    sprout_register_receive_callback(on_data_received);
    sprout_start();
    
    // Start discovery
    sprout_start_loop_task(portMAX_DELAY, false);
    sprout_start_reconnect_task();
    
    // Send periodic broadcasts
    while (1) {
        char msg[] = "Hello from host!";
        sprout_broadcast(msg, strlen(msg), 1);
        
        // Print stats
        sprout_network_stats_t stats;
        sprout_get_network_stats(&stats);
        printf("Peers: %zu, Sent: %lu, Received: %lu\n",
               stats.peer_count, stats.packets_sent, stats.packets_received);
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

---

## LTS Functions

Long-Term Support functions for version management and compatibility checking.

### `sprout_lts_version_to_string()`

Convert a version number to a human-readable string.

```c
void sprout_lts_version_to_string(code_version_t version, char *buf, size_t buf_size);
```

**Parameters:**
- `version` - Version to convert
- `buf` - Buffer to store string (recommend at least 16 bytes)
- `buf_size` - Size of buffer

**Example:**
```c
char ver_str[16];
sprout_lts_version_to_string(SPROUT_PROTOCOL_VERSION, ver_str, sizeof(ver_str));
printf("Protocol version: %s\n", ver_str);  // Output: "1.0.0"
```

---

### `sprout_lts_log_compatibility()`

Log compatibility information for debugging.

```c
void sprout_lts_log_compatibility(code_version_t remote_version);
```

**Parameters:**
- `remote_version` - Version received from remote device

**Notes:**
- Logs to ESP_LOG with appropriate level (INFO, WARN, or ERROR)
- Useful for debugging version mismatch issues

---

### `sprout_lts_get_min_supported_version()`

Get the minimum supported protocol version.

```c
code_version_t sprout_lts_get_min_supported_version(void);
```

**Returns:**
- Minimum version that this device can communicate with

---

### `sprout_lts_supports_feature()`

Check if a specific feature is supported by a given version.

```c
bool sprout_lts_supports_feature(code_version_t version, const char *feature);
```

**Parameters:**
- `version` - Protocol version to check
- `feature` - Feature name string

**Supported Features:**
- `"fragmentation"` - Packet fragmentation support
- `"mesh_routing"` - Mesh/extender routing support
- `"versioning"` - Protocol version field support

**Returns:**
- `true` if the version supports the feature, `false` otherwise

**Example:**
```c
code_version_t peer_version = ...; // from peer handshake
if (sprout_lts_supports_feature(peer_version, "fragmentation")) {
    // Can send fragmented packets to this peer
}
```

---

### Version Checking Macros

Static inline functions for quick version checks:

```c
// Check if remote version is compatible
bool sprout_version_is_compatible(code_version_t remote_version);

// Check if remote version matches current (no special handling needed)
bool sprout_version_is_current(code_version_t remote_version);

// Check if remote version needs legacy handler
bool sprout_version_needs_legacy_handler(code_version_t remote_version);

// Check if remote version is newer than ours
bool sprout_version_needs_newer_handler(code_version_t remote_version);

// Get current protocol version
code_version_t sprout_get_current_version(void);
```

---

## See Also

- [README.md](../README.md) - Project overview
- [CONFIGURATION.md](CONFIGURATION.md) - Kconfig options
- [Test Suite](../test/README_TESTS.md) - Testing documentation
- [Quick Start](../test/QUICK_START.md) - Quick setup guide
