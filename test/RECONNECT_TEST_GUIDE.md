# FPR Reconnect Feature - Test Guide

## Overview
The FPR library now includes **persistent reconnect/keepalive monitoring** that maintains connections indefinitely, even after discovery loops complete.

## What's New

### Core Changes
1. **Persistent background tasks** for both host and client modes
2. **Automatic keepalive** sends every 3 seconds to maintain connection
3. **Timeout detection** - marks peer disconnected after 5 seconds of inactivity
4. **Auto-reconnection** - client automatically rediscovers and reconnects to host

### Configuration
```c
// Timing constants (in fpr.c)
#define FPR_RECONNECT_TIMEOUT_MS 5000      // Disconnect after 5s of no traffic
#define FPR_KEEPALIVE_INTERVAL_MS 3000     // Send keepalive every 3s
```

### New API Functions
```c
// Start persistent reconnect monitoring (call after discovery loop)
esp_err_t fpr_network_start_reconnect_task(void);

// Stop reconnect monitoring
esp_err_t fpr_network_stop_reconnect_task(void);

// Check if reconnect task is running
bool fpr_network_is_reconnect_task_running(void);
```

## Test Files Updated

### `test_fpr_client.c`
- Registers data receive callback
- Starts 20s discovery loop
- **After loop completes**: starts reconnect task
- Sends periodic test messages to host
- Will automatically reconnect if host comes back after disconnect

### `test_fpr_host.c`
- Registers data receive callback  
- Starts 20s broadcast loop
- **After loop completes**: starts reconnect task
- Sends periodic test messages to all connected clients
- Automatically detects stale clients and marks them disconnected

## How to Test

### Build and Flash
```powershell
# Clean build
idf.py fullclean
idf.py build

# Flash to two devices
idf.py -p COM3 flash monitor  # Host device
idf.py -p COM4 flash monitor  # Client device
```

### Test Scenario 1: Normal Connection
1. Start **host** device first
2. Start **client** device
3. **Expected behavior**:
   - Client discovers host within 20s
   - Connection established automatically
   - After loops finish, reconnect task starts
   - Keepalive messages sent every 3s
   - Periodic test messages exchanged

**Serial logs to watch for**:
```
[FPR_CLIENT_TEST] [LOOP] Discovery loop completed
[FPR_CLIENT_TEST] [RECONNECT] Starting persistent reconnect task...
[FPR_CLIENT_TEST] [RECONNECT] Reconnect task started - connections will be maintained indefinitely
[fpr] Reconnect task started for client mode
```

### Test Scenario 2: Reconnection After Disconnect
1. Establish connection (scenario 1)
2. **Power-cycle the host** or disable its WiFi
3. **Expected behavior**:
   - Client detects host timeout after ~5s
   - Client logs: `Host timed out (age XXXX ms) - marking disconnected for reconnect`
   - Client continues running
4. **Power host back on**
5. **Expected behavior**:
   - Host starts broadcasting again
   - Client rediscovers host automatically
   - Connection re-established
   - Messages resume

**Serial logs to watch for (client)**:
```
[fpr] Host timed out (age 5234 ms) - marking disconnected for reconnect
[FPR_CLIENT_TEST] [DISCOVERY] Host found #1: FPR-Host-Test (...)
[fpr] Connection established with FPR-Host-Test, key: 0xXXXXXXXX
```

### Test Scenario 3: Reconnection After Client Disconnect
1. Establish connection (scenario 1)
2. **Power-cycle the client** or disable its WiFi
3. **Expected behavior**:
   - Host detects client timeout after ~5s
   - Host logs: `Client XX:XX:XX:XX:XX:XX timed out (age XXXX ms) - disconnecting`
   - Host continues monitoring for new connections
4. **Power client back on**
5. **Expected behavior**:
   - Client starts discovery loop
   - Client finds host and connects
   - Host accepts reconnection
   - Messages resume

**Serial logs to watch for (host)**:
```
[fpr] Client XX:XX:XX:XX:XX:XX timed out (age 5123 ms) - disconnecting
[fpr] Processing connection request from FPR-Client-Test, key: 0x00000000, visibility: 0
[fpr] Peer connected: FPR-Client-Test (XX:XX:XX:XX:XX:XX)
```

### Test Scenario 4: Long-term Stability
1. Establish connection (scenario 1)
2. Let both devices run for **1+ hours**
3. **Expected behavior**:
   - Connection remains stable
   - Keepalive messages sent continuously every 3s
   - No unexpected disconnections
   - Statistics tasks show ongoing message exchange

**Monitor statistics output** (every 10s):
```
========== STATISTICS ==========
Connected: YES
Host: FPR-Host-Test (XX:XX:XX:XX:XX:XX)
Messages sent: 120
Messages received: 120
================================
```

## Debugging Tips

### If reconnection doesn't work:
1. **Check logs for reconnect task start**:
   ```
   [fpr] Reconnect task started for client mode
   ```
   If not present, ensure `fpr_network_start_reconnect_task()` is called after loop.

2. **Check keepalive interval**:
   - Should see periodic sends (not logged by default for keepalives)
   - Can add debug logs in `_fpr_client_reconnect_task` / `_fpr_host_reconnect_task`

3. **Verify timeout detection**:
   - After disconnect, should see timeout message within 5-8s
   - If not, check `FPR_RECONNECT_TIMEOUT_MS` value

4. **Check discovery after timeout**:
   - Client should continue processing broadcasts even after marking host disconnected
   - Host should continue accepting unicast connection requests

### Common Issues

**Client doesn't reconnect**:
- Ensure client discovery callback (`_handle_client_discovery`) processes broadcasts even when peer already exists but is disconnected
- Check that `existing->state` is set back to `FPR_PEER_STATE_DISCOVERED` on timeout

**Host doesn't accept reconnection**:
- Verify host receive handler accepts unicast from previously connected peers
- Check that peer isn't marked as `BLOCKED` after first disconnect

**Keepalive floods logs**:
- Keepalive sends use `ESP_LOGD` (debug level) by default
- Set log level: `esp_log_level_set("fpr", ESP_LOG_INFO);` to reduce noise

## Performance Notes

- **CPU overhead**: Minimal (~1% per device)
- **Network overhead**: ~40 bytes every 3s per peer
- **Memory**: +3KB stack per reconnect task
- **Power**: Slightly higher due to periodic sends (negligible for mains-powered devices)

## Configuration Tuning

Adjust timing in `fpr.c` for your use case:

```c
// More aggressive (faster detection, more network traffic)
#define FPR_RECONNECT_TIMEOUT_MS 3000
#define FPR_KEEPALIVE_INTERVAL_MS 2000

// More relaxed (slower detection, less network traffic)
#define FPR_RECONNECT_TIMEOUT_MS 10000
#define FPR_KEEPALIVE_INTERVAL_MS 5000
```

Remember: `KEEPALIVE_INTERVAL` should be **less than** `RECONNECT_TIMEOUT` by at least 1-2 seconds to ensure keepalives arrive before timeout.

## Next Steps

After confirming basic reconnection works:
1. Test with multiple clients (5-10) connecting to one host
2. Test rapid connect/disconnect cycles
3. Test reconnection while actively exchanging data
4. Test reconnection with poor signal (RSSI < -70 dBm)
5. Add reconnection event callbacks if needed for your application

---

**Last Updated**: November 23, 2025
