# FPR Data Size Test Guide

## Overview

The data size test validates FPR's ability to send and receive various payload sizes with **byte-perfect accuracy**. It tests the following payload sizes (in bytes):

**Small packets (< CHUNK_CAP):**
- 50 bytes

**Single-chunk packets (≈ CHUNK_CAP):**
- 100 bytes

**Multi-chunk packets:**
- 150, 200, 250, 300, 350, 400, 450, 500 bytes
- 600, 700, 750, 800, 850, 900, 950, 1000 bytes

Each test payload includes:
- **Header** (4 bytes): test_id (2 bytes) + size (2 bytes)
- **Payload**: deterministic pattern (index + seed) for exact verification

## Verification Modes

### Strict Byte-by-Byte Verification (Default)
When `CONFIG_FPR_DATA_SIZE_TEST_VERIFY_PAYLOAD` is enabled (default), every single byte is verified:
- Regenerates expected payload using same deterministic seed
- Compares byte-by-byte with received data
- Reports exact offset and values of any mismatches
- **Ensures data integrity over-the-air**

### Header-Only Verification
When disabled, only checks:
- Test ID matches
- Size matches
- Faster but less thorough

## Quick Start

### Option 1: Using Kconfig (Recommended)

1. Configure the test mode:
   ```bash
   idf.py menuconfig
   ```

2. Navigate to: `Component config` → `fpr`
   - Enable `Enable FPR Test Mode`
   - Select `Data Size Test` from `Select FPR Test Role`

3. Configure test parameters in `Data Size Test Configuration`:
   - **Data Size Test Mode**: Choose `Host (Receiver/Echo)` or `Client (Sender)`
   - **Enable Auto Connection Mode**: ON (recommended)
   - **Enable Echo Mode**: ON (for round-trip verification)
   - **Test Interval (ms)**: 2000 (default)
   - **Receive Timeout (ms)**: 5000 (default)
   - **Enable Strict Payload Verification**: ON (byte-by-byte verification)

4. Enable auto-start:
   - Enable `Enable FPR Test Auto Start`

5. Build and flash:
   ```bash
   idf.py build
   idf.py -p COMx flash monitor
   ```

### Option 2: Using Compile Definitions

**For HOST:**
```cmake
# In examples/default/main/CMakeLists.txt
target_compile_definitions(${COMPONENT_LIB} PRIVATE 
    FPR_TEST_DATA_SIZES
    FPR_TEST_HOST
    FPR_TEST_AUTO_START
)
```

**For CLIENT:**
```cmake
# In examples/default/main/CMakeLists.txt
target_compile_definitions(${COMPONENT_LIB} PRIVATE 
    FPR_TEST_DATA_SIZES
    FPR_TEST_AUTO_START
)
```

## Test Configuration

### Default Settings
```c
fpr_data_size_test_config_t config = {
    .auto_mode = true,         // Automatic connection
    .test_interval_ms = 2000,  // 2 second delay between tests
    .echo_mode = true          // Host echoes data back to client
};
```

### Custom Configuration

**HOST:**
```c
#include "test_fpr_data_sizes.h"

fpr_data_size_test_config_t cfg = {
    .auto_mode = true,
    .test_interval_ms = 2000,
    .echo_mode = true  // Host will echo received data back
};
fpr_data_size_test_host_start(&cfg);
```

**CLIENT:**
```c
#include "test_fpr_data_sizes.h"

fpr_data_size_test_config_t cfg = {
    .auto_mode = true,
    .test_interval_ms = 2000,
    .echo_mode = true  // Client will wait for and verify echoes
};
fpr_data_size_test_client_start(&cfg);
```

## Test Workflow

### Without Echo Mode (`echo_mode = false`)
1. Host starts and waits for connections
2. Client connects to host
3. Client sends all test sizes sequentially
4. Host receives and verifies each payload
5. Test completes after all sizes sent

### With Echo Mode (`echo_mode = true`)
1. Host starts and waits for connections
2. Client connects to host
3. Client sends test payload
4. Host receives, verifies, and echoes back
5. Client receives echo and verifies
6. Repeat for all test sizes

## Expected Output

### Client Output Example
```
I (5123) FPR_DATA_SIZE_TEST: [CLIENT] Connected to host 24:6F:28:XX:XX:XX
I (5234) FPR_DATA_SIZE_TEST: [CLIENT] Sending test #1: 50 bytes...
I (5345) FPR_DATA_SIZE_TEST: [CLIENT] ✓ Test #1 sent successfully (50 bytes)
I (5456) FPR_DATA_SIZE_TEST: [CLIENT] Waiting for echo response...
I (5567) FPR_DATA_SIZE_TEST: [VERIFY] Payload verified: test_id=1, size=50 bytes
I (5678) FPR_DATA_SIZE_TEST: [CLIENT] ✓ Echo verified for test #1
...
I (45123) FPR_DATA_SIZE_TEST: [CLIENT] All tests completed. Passed: 18, Failed: 0
```

### Host Output Example
```
I (3456) FPR_DATA_SIZE_TEST: [HOST] Waiting for client connections and data...
I (5234) FPR_DATA_SIZE_TEST: [RX] Received 50 bytes from 24:6F:28:XX:XX:XX (test_id=1)
I (5345) FPR_DATA_SIZE_TEST: [VERIFY] Payload verified: test_id=1, size=50 bytes
I (5456) FPR_DATA_SIZE_TEST: [RX] ✓ Test #1 PASSED (50 bytes)
I (5567) FPR_DATA_SIZE_TEST: [ECHO] Sending 50 bytes back to client...
I (5678) FPR_DATA_SIZE_TEST: [ECHO] Echo sent successfully
...
```

## Statistics

Every 10 seconds, statistics are printed:

```
=== DATA SIZE TEST STATS ===
  Tests Passed:    18
  Tests Failed:    0
  Bytes Sent:      9650
  Bytes Received:  9650
===========================
  FPR Stats:
    Packets Sent:      234
    Packets Received:  234
    Packets Dropped:   0
    Send Failures:     0
```

## Retrieving Stats Programmatically

```c
uint32_t passed, failed, sent, received;
fpr_data_size_test_get_stats(&passed, &failed, &sent, &received);

ESP_LOGI(TAG, "Tests: %lu passed, %lu failed", passed, failed);
ESP_LOGI(TAG, "Bytes: %lu sent, %lu received", sent, received);
```

## Stopping the Test

```c
fpr_data_size_test_stop();
```

## Troubleshooting

### Test Failures

**Symptom:** "Size mismatch" or "Test ID mismatch"
- **Cause:** Packet corruption or fragmentation issue
- **Fix:** Check buffer sizes, ensure `fpr_network_get_data_from_peer()` is working correctly

**Symptom:** "No echo response"
- **Cause:** Timeout or connection dropped
- **Fix:** Increase timeout in `fpr_network_get_data_from_peer()`, check connection stability

**Symptom:** Tests pass but bytes don't match
- **Cause:** Truncation in `fpr_network_get_data_from_peer()`
- **Fix:** Verify buffer allocation matches expected payload size

### Connection Issues

**Symptom:** Client never connects
- Check WiFi is initialized
- Verify host is broadcasting
- Check security handshake logs

**Symptom:** Connection drops during test
- Check keepalive settings
- Verify both devices are within range
- Check for interference

## Test Sizes Summary

Total bytes tested per run: **9,650 bytes**

| Test # | Size (bytes) | Packets Expected |
|--------|--------------|------------------|
| 1      | 50           | 1 (SINGLE)       |
| 2      | 100          | 1 (SINGLE)       |
| 3      | 150          | 2 (START+END)    |
| 4      | 200          | 2                |
| 5      | 250          | 3                |
| 6      | 300          | 3                |
| 7      | 350          | 4                |
| 8      | 400          | 4                |
| 9      | 450          | 5                |
| 10     | 500          | 5                |
| 11     | 600          | 6                |
| 12     | 700          | 7                |
| 13     | 750          | 8                |
| 14     | 800          | 8                |
| 15     | 850          | 9                |
| 16     | 900          | 9                |
| 17     | 950          | 10               |
| 18     | 1000         | 10               |

**Note:** Packet count assumes ~100 byte chunk capacity. Actual fragmentation depends on `sizeof(fpr_package_t::protocol)`.
