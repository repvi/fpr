# Sprout Unit Tests

Pure logic unit tests for the Sprout network protocol that run without requiring hardware or ESP-IDF components.

## Overview

These unit tests verify the core logic of the Sprout protocol including:
- Helper functions (string operations, broadcast address checks)
- Peer management (add, remove, lookup)
- Packet handling (fragmentation, packet types)
- State management (network states, power modes, queue modes)

## Purpose

These tests are designed to:
- Speed up development by testing logic without flashing devices
- Catch bugs early in the development cycle
- Verify core algorithms work correctly
- Provide regression testing for future changes

## Test Structure

```
unit_test/
├── CMakeLists.txt           # Build configuration
├── mocks/
│   ├── esp_mock.h          # Mock ESP-IDF headers
│   └── esp_mock.c          # Mock ESP-IDF implementations
├── test_helpers.c          # Tests for helper functions
├── test_peer_management.c  # Tests for peer management logic
├── test_packet_handling.c  # Tests for packet handling logic
└── test_state_management.c # Tests for state management logic
```

## Running the Tests

### Option 1: Standalone Host Tests (No Hardware Required)

**Fastest option - runs on your host machine without ESP-IDF**

**Using Makefile (Recommended):**
```bash
cd unit_test
make run
```

**Or manually:**
1. **Compile the standalone test runner:**
   ```bash
   gcc -o unit_test/standalone_test unit_test/standalone_test.c -Wall
   ```

2. **Run the tests:**
   ```bash
   ./unit_test/standalone_test
   ```

**Available make targets:**
- `make` - Build the test executable
- `make run` - Build and run tests
- `make clean` - Remove the test executable

This will run 27 core logic tests covering:
- String operations and broadcast address detection
- Peer state transitions
- Network state management
- Power and queue modes
- Package types
- Sequence number handling
- Hashmap operations
- Network statistics
- MAC address operations
- **Client mode logic** (discovery, auto-connection)
- **Host mode logic** (peer management, max peers)
- **Extender mode logic** (packet forwarding, hop counts)
- **Cross-mode operations** (mode switching, security handshake, keepalive tracking)

**Expected Output:**
```
=== Sprout Unit Tests (Standalone) ===

Running test_safe_string_copy_normal... PASSED
...
=== Test Results ===
Total: 15
Passed: 15
Failed: 0

All tests PASSED!
```

### Option 2: ESP-IDF Unit Tests (Requires ESP-IDF)

**For more comprehensive testing with Unity framework**

#### Prerequisites

- ESP-IDF development environment
- Unity test framework (included in ESP-IDF)

#### Build and Run

1. **Build the unit test component:**
   ```bash
   idf.py build
   ```

2. **Run all unit tests:**
   ```bash
   idf.py flash monitor
   ```

3. **Run specific test file:**
   Modify the `main()` function in the test file to only run specific tests.

### Expected Output

```
.....
8 Tests 0 Failures 0 Ignored
OK
```

## Test Coverage

### test_helpers.c
- `test_safe_string_copy_normal` - Normal string copy operation
- `test_safe_string_copy_truncate` - String truncation handling
- `test_safe_string_copy_null_dest` - Null destination handling
- `test_safe_string_copy_null_src` - Null source handling
- `test_safe_string_copy_zero_size` - Zero size handling
- `test_is_broadcast_address_true` - Broadcast address detection
- `test_is_broadcast_address_false` - Non-broadcast address detection
- `test_is_broadcast_address_partial` - Partial broadcast address detection

### test_peer_management.c
- `test_add_peer_internal_basic` - Basic peer addition
- `test_add_peer_internal_null_mac` - Null MAC address handling
- `test_add_peer_internal_duplicate` - Duplicate peer handling
- `test_remove_peer_internal` - Peer removal
- `test_remove_peer_internal_not_found` - Remove non-existent peer
- `test_copy_peer_to_info` - Peer info copying
- `test_add_discovered_peer` - Discovered peer addition
- `test_peer_count` - Peer count verification

### test_packet_handling.c
- `test_package_type_single` - Single packet type
- `test_package_type_fragmented` - Fragmented packet types
- `test_package_size_compatibility` - Package size validation
- `test_package_mac_addresses` - MAC address handling
- `test_package_hop_count` - Hop count tracking
- `test_package_id_control` - Control packet ID
- `test_package_version` - Protocol version
- `test_package_payload_size_limit` - Payload size limits
- `test_queue_mode_normal` - Normal queue mode
- `test_queue_mode_latest_only` - Latest-only queue mode
- `test_peer_state_transitions` - Peer state changes

### test_state_management.c
- `test_network_state_uninitialized` - Uninitialized state
- `test_network_state_initialized` - Initialized state
- `test_network_state_started` - Started state
- `test_network_state_paused` - Paused state
- `test_network_state_stopped` - Stopped state
- `test_power_mode_normal` - Normal power mode
- `test_power_mode_low` - Low power mode
- `test_queue_mode_normal` - Queue mode normal
- `test_queue_mode_latest_only` - Queue mode latest only
- `test_channel_setting` - Channel configuration
- `test_channel_zero_auto` - Auto channel selection
- `test_visibility_public` - Public visibility
- `test_visibility_private` - Private visibility
- `test_mode_client` - Client mode
- `test_mode_host` - Host mode
- `test_mode_extender` - Extender mode
- `test_sequence_number_increment` - Sequence number handling
- `test_sequence_number_overflow` - Sequence overflow
- `test_network_stats_initialization` - Stats initialization
- `test_network_stats_increment` - Stats increment
- `test_paused_flag` - Pause flag
- `test_mac_address_storage` - MAC storage
- `test_network_name_storage` - Network name storage
- `test_network_name_truncation` - Name truncation

## Adding New Tests

1. Create a new test file in `unit_test/`
2. Include necessary headers:
   ```c
   #include "unity.h"
   #include "../internal_include/sprout/internal/private_defs.h"
   #include "../internal_include/sprout/internal/helpers.h"
   ```
3. Implement `setUp()` and `tearDown()` functions
4. Write test functions with `test_` prefix
5. Add test runs to `main()` function
6. Add the test file to `UNIT_TEST_SOURCES` in `CMakeLists.txt`

## Mock Functions

The `mocks/esp_mock.h` and `mocks/esp_mock.c` provide mock implementations for:
- ESP-NOW functions
- ESP error codes
- MAC address types
- WiFi types

These mocks allow tests to run without actual ESP-IDF hardware.

## Limitations

- These tests do not test actual ESP-NOW communication
- Hardware-specific behavior (timing, interrupts) is not tested
- Integration with ESP-IDF components is not tested
- Real-world network conditions are not simulated

For full integration testing, use the existing test files in the `test/` directory which require actual hardware.

## Continuous Integration

These unit tests can be integrated into CI/CD pipelines to:
- Run on every commit
- Catch regressions early
- Provide fast feedback without hardware

## Troubleshooting

### Build Errors
- Ensure ESP-IDF environment is properly set up
- Check that Unity framework is available in your ESP-IDF version
- Verify include paths in `CMakeLists.txt`

### Test Failures
- Check that mocks are properly initialized
- Verify test isolation (no state leakage between tests)
- Review test assertions for correctness

### Linking Errors
- Ensure all required source files are in `UNIT_TEST_SOURCES`
- Check that internal headers are accessible
- Verify that the component is properly registered
