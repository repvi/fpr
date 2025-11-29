# FPR Library Tests

This directory contains test programs for the FPR (Firmware Peer-to-Peer Routing) library.

## Test Files

### 1. `test_fpr_host.c`
Tests the FPR library in Host mode.

**Features:**
- Automatic or manual connection mode (configurable)
- Peer discovery with callbacks
- Connection approval/rejection (manual mode)
- Data echo functionality
- Periodic statistics and peer listing
- Broadcast messaging

**Configuration:**
```c
#define TEST_AUTO_MODE      1  // 1=auto, 0=manual
#define TEST_MAX_PEERS      5  // Max connected peers
#define TEST_ECHO_ENABLED   1  // Echo received data back
```

**How to Run:**
1. Compile: Use build script `.\build_test.ps1 host` or edit `components/fpr/test/CMakeLists.txt`
2. Flash to Device 1
3. Monitor serial output
4. Watch for client connections

**Expected Output:**
```
[FPR_HOST_TEST] FPR Host is now RUNNING
[FPR_HOST_TEST] [DISCOVERY] Peer #1: FPR-Client-Test (xx:xx:xx:xx:xx:xx) RSSI: -45 dBm
[FPR_HOST_TEST] [CONNECT] Peer connected: FPR-Client-Test
[FPR_HOST_TEST] [DATA] Message #1 from xx:xx:xx:xx:xx:xx (hops: 0, size: 25 bytes)
[FPR_HOST_TEST] [ECHO] Echo sent successfully
```

### 2. `test_fpr_client.c`
Tests the FPR library in Client mode.

**Features:**
- Automatic or manual connection mode (configurable)
- Host discovery and scanning
- Automatic reconnection
- Periodic message sending
- Connection statistics
- Best host selection (manual mode)

**Configuration:**
```c
#define TEST_AUTO_MODE          1     // 1=auto, 0=manual
#define TEST_SCAN_DURATION_MS   5000  // Scan time (manual mode)
#define TEST_MESSAGE_INTERVAL_MS 5000 // Message frequency
```

**How to Run:**
1. Ensure a host is running first
2. Compile: Use build script `.\build_test.ps1 client` or edit `components/fpr/test/CMakeLists.txt`
3. Flash to Device 2
4. Monitor serial output
5. Watch connection and messaging

**Expected Output:**
```
[FPR_CLIENT_TEST] FPR Client is now RUNNING
[FPR_CLIENT_TEST] [DISCOVERY] Host found #1: FPR-Host-Test (xx:xx:xx:xx:xx:xx) RSSI: -50 dBm
[FPR_CLIENT_TEST] [CONNECT] *** Connected to host: FPR-Host-Test ***
[FPR_CLIENT_TEST] [SEND] Sending message: "Test message #1 from client"
[FPR_CLIENT_TEST] [SEND] Message sent successfully
[FPR_CLIENT_TEST] [DATA] Message #1 (echo response)
```

### 3. `test_fpr_extender.c`
Tests the FPR library in Extender mode.

**Features:**
- Message relay monitoring
- Relay statistics
- Minimal configuration

**How to Run:**
1. Ensure host and client are running
2. Compile: Use build script `.\build_test.ps1 extender` or edit `components/fpr/test/CMakeLists.txt`
3. Flash to Device 3
4. Place device between host and client
5. Monitor relay activity

**Expected Output:**
```
[FPR_EXTENDER_TEST] FPR Extender is now RUNNING
[FPR_EXTENDER_TEST] [RELAY] Message #1
[FPR_EXTENDER_TEST]   From: xx:xx:xx:xx:xx:xx
[FPR_EXTENDER_TEST]   To: yy:yy:yy:yy:yy:yy
[FPR_EXTENDER_TEST]   Hops: 1
```

## Test Scenarios

### Scenario 1: Basic Host-Client Communication
1. Flash Device 1 with `test_fpr_host.c` (auto mode)
2. Flash Device 2 with `test_fpr_client.c` (auto mode)
3. Power both devices
4. Observe automatic connection and message exchange

### Scenario 2: Manual Connection with Approval
1. Flash Device 1 with `test_fpr_host.c` (manual mode)
2. Flash Device 2 with `test_fpr_client.c` (manual mode)
3. Observe host scanning and approval process
4. Watch connection establishment

### Scenario 3: Multi-Hop Mesh Network
1. Flash Device 1 with `test_fpr_host.c`
2. Flash Device 2 with `test_fpr_extender.c`
3. Flash Device 3 with `test_fpr_client.c`
4. Place devices in line (Host -> Extender -> Client)
5. Observe messages routing through extender

### Scenario 4: Multiple Clients
1. Flash Device 1 with `test_fpr_host.c` (max_peers = 5)
2. Flash Devices 2-4 with `test_fpr_client.c`
3. Observe multiple clients connecting
4. Watch host broadcast messages to all

### Scenario 5: Connection Stress Test
1. Flash Device 1 with `test_fpr_host.c`
2. Flash Device 2 with `test_fpr_client.c`
3. Power cycle client device repeatedly
4. Observe reconnection behavior and statistics

## Modifying Tests

### Change Connection Mode
```c
// In test_fpr_host.c or test_fpr_client.c
#define TEST_AUTO_MODE 0  // Change to 0 for manual mode
```

### Adjust Message Frequency
```c
// In test_fpr_client.c
#define TEST_MESSAGE_INTERVAL_MS 1000  // Send every 1 second
```

### Change Peer Limit
```c
// In test_fpr_host.c
#define TEST_MAX_PEERS 10  // Allow up to 10 clients
```

### Disable Echo
```c
// In test_fpr_host.c
#define TEST_ECHO_ENABLED 0  // Don't echo back
```

## Compiling Tests

### Method 1: Using Build Scripts (Recommended)

**PowerShell (Windows):**
```powershell
# Build host test
.\build_test.ps1 host

# Build client test
.\build_test.ps1 client

# Build extender test
.\build_test.ps1 extender

# Return to normal build
.\build_test.ps1 main

# Show help
.\build_test.ps1 help
```

**Bash (Linux/Mac):**
```bash
# Make script executable (first time only)
chmod +x build_test.sh

# Build host test
./build_test.sh host

# Build client test
./build_test.sh client

# Build extender test
./build_test.sh extender

# Return to normal build
./build_test.sh main
```

The build scripts automatically:
1. Configure `components/fpr/test/CMakeLists.txt`
2. Build the selected test
3. Display success message with flash instructions

### Method 2: Manual CMake Configuration

Edit `components/fpr/test/CMakeLists.txt` and uncomment the desired test:

```cmake
# Select which test to build (uncomment ONE):
set(TEST_SRCS
    "test_fpr_host.c"      # Uncomment for Host test
    # "test_fpr_client.c"    # Uncomment for Client test
    # "test_fpr_extender.c"  # Uncomment for Extender test
)
```

Then build normally:
```bash
idf.py build flash monitor
```

### Method 3: Replace main.cpp (Legacy)
```bash
# Backup original main
cp main/main.cpp main/main.cpp.backup

# Copy test file
cp components/fpr/test/test_fpr_host.c main/main.cpp

# Build
idf.py build flash monitor

# Restore original main
cp main/main.cpp.backup main/main.cpp
```

## Troubleshooting

**No connection:**
- Ensure both devices are powered and in range
- Check WiFi is initialized properly
- Verify `fpr_network_start()` is called on both
- Check serial output for error messages

**Messages not received:**
- Verify callbacks are set correctly
- Check message size < 45 bytes
- Ensure devices are connected (check statistics)

**Compilation errors:**
- Verify FPR library is in `components/fpr/`
- Check `CMakeLists.txt` includes FPR component
- Ensure ESP-IDF environment is activated

**Frequent disconnections:**
- Check signal strength (RSSI)
- Reduce distance between devices
- Check for WiFi interference

## Expected Performance

- **Discovery Time**: 1-3 seconds
- **Connection Time**: 1-5 seconds (auto mode)
- **Message Latency**: <100ms (direct), <200ms (1 hop)
- **Throughput**: ~10-50 messages/second (depends on size)
- **Range**: 50-200m line-of-sight (varies by environment)

## Logging Levels

Adjust log verbosity in `sdkconfig`:
```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y  # For verbose output
```

Or at runtime:
```c
esp_log_level_set("FPR_HOST_TEST", ESP_LOG_DEBUG);
esp_log_level_set("FPR", ESP_LOG_DEBUG);
```

## Next Steps

After verifying basic functionality:
1. Test with custom message payloads
2. Implement application-specific protocols
3. Test multi-hop routing with 3+ devices
4. Measure power consumption
5. Test in different environments (indoor/outdoor)
6. Stress test with many clients
7. Test reconnection after range loss
