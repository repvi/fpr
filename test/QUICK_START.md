# FPR Test Quick Reference

## Building Tests

### Windows (PowerShell)
```powershell
.\build_test.ps1 host      # Host test
.\build_test.ps1 client    # Client test
.\build_test.ps1 extender  # Extender test
.\build_test.ps1 main      # Return to normal build
```

### Linux/Mac (Bash)
```bash
./build_test.sh host       # Host test
./build_test.sh client     # Client test
./build_test.sh extender   # Extender test
./build_test.sh main       # Return to normal build
```

## Flashing and Monitoring

After building:
```bash
idf.py flash monitor
```

Or combined with build:
```bash
idf.py build flash monitor
```

## Test Configuration

Edit the test file before building to change behavior:

### Host Test (`test/test_fpr_host.c`)
```c
#define TEST_AUTO_MODE      1  // 1=auto, 0=manual
#define TEST_MAX_PEERS      5  // Max peers (0=unlimited)
#define TEST_ECHO_ENABLED   1  // Echo data back
```

### Client Test (`test/test_fpr_client.c`)
```c
#define TEST_AUTO_MODE          1     // 1=auto, 0=manual
#define TEST_SCAN_DURATION_MS   5000  // Scan time
#define TEST_MESSAGE_INTERVAL_MS 5000 // Message frequency
```

## Quick Test Setup

### Two-Device Test
1. **Device 1 (Host):**
   ```bash
   .\build_test.ps1 host
   idf.py flash monitor
   ```

2. **Device 2 (Client):**
   ```bash
   .\build_test.ps1 client
   idf.py flash monitor
   ```

### Three-Device Mesh Test
1. **Device 1 (Host):**
   ```bash
   .\build_test.ps1 host
   idf.py flash monitor
   ```

2. **Device 2 (Extender):**
   ```bash
   .\build_test.ps1 extender
   idf.py flash monitor
   ```

3. **Device 3 (Client):**
   ```bash
   .\build_test.ps1 client
   idf.py flash monitor
   ```

## Returning to Normal Build

When done testing:
```bash
.\build_test.ps1 main
idf.py build flash monitor
```

## Troubleshooting

**Build fails with "component not found":**
- Ensure you're in the firmware directory
- Check that `components/fpr/test/CMakeLists.txt` exists

**Test doesn't run:**
- Verify the correct test is uncommented in `components/fpr/test/CMakeLists.txt`
- Check serial output for initialization errors

**Connection issues:**
- Ensure both devices are in range
- Check WiFi is initialized (should see in logs)
- Verify both devices called `fpr_network_start()`

## File Structure

```
firmware/
├── build_test.ps1          # Windows build script
├── build_test.sh           # Linux/Mac build script
└── components/fpr/
    ├── fpr.h               # FPR API
    ├── fpr.c               # FPR implementation
    ├── README.md           # FPR usage guide
    └── test/
        ├── CMakeLists.txt      # Test component config
        ├── test_fpr_host.c     # Host test
        ├── test_fpr_client.c   # Client test
        ├── test_fpr_extender.c # Extender test
        ├── README_TESTS.md     # Full documentation
        └── QUICK_START.md      # This file
```

## Next Steps

1. Build and flash host test
2. Build and flash client test on another device
3. Monitor serial output on both devices
4. Observe automatic connection and messaging
5. Experiment with configuration options
6. Try manual mode for connection control
7. Add third device as extender for mesh testing

For detailed information, see `components/fpr/test/README_TESTS.md`
