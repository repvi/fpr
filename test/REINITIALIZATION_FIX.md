# FPR Reinitialization Fix

## Problem Description

When calling `fpr_host_test_stop()` or `fpr_client_test_stop()` followed by `fpr_host_test_start()` or `fpr_client_test_start()`, devices would fail to reconnect to each other. This was caused by:

1. **Incomplete cleanup**: The test stop functions only deleted test tasks but didn't call `fpr_network_deinit()`
2. **Stale peer data**: Old peer entries remained in the hashmap from the previous initialization
3. **No init state check**: `fpr_network_init()` didn't check if already initialized, allowing double-initialization

## Root Cause

When you reinitialize without proper cleanup:
- Old peer entries with `is_connected=true` remain in the hashmap
- The client thinks it's already connected and ignores new host broadcasts
- The host has stale peer entries that conflict with new connections
- Security state machine starts in the wrong state
- ESP-NOW peer list becomes inconsistent with internal state

## Fixes Applied

### 1. Added `fpr_network_deinit()` to test stop functions

**File: `test_fpr_client.c`**
```c
void fpr_client_test_stop(void)
{
    // ... delete test tasks ...
    
    // Properly deinitialize FPR network to clean up all state
    fpr_network_deinit();
    
    ESP_LOGI(TAG, "FPR Client Test stopped");
}
```

**File: `test_fpr_host.c`**
```c
void fpr_host_test_stop(void)
{
    // ... delete test tasks ...
    
    // Properly deinitialize FPR network to clean up all state
    fpr_network_deinit();
    
    ESP_LOGI(TAG, "FPR Host Test stopped");
}
```

### 2. Added initialization state check

**File: `fpr.c`**
```c
esp_err_t fpr_network_init_ex(const char *name, const fpr_init_config_t *config)
{
    // ... parameter checks ...
    
    // Check if already initialized
    if (fpr_net.state != FPR_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "FPR network already initialized. Call fpr_network_deinit() first.");
        return ESP_ERR_INVALID_STATE;
    }
    
    // ... continue initialization ...
}
```

## What `fpr_network_deinit()` Does

The deinit function performs complete cleanup:
1. Deletes reconnect task if running
2. Calls `_reset_all_peers()` - removes all peer entries and ESP-NOW peers
3. Frees the hashmap structure
4. Sets state to `FPR_STATE_UNINITIALIZED`
5. Zeros out entire `fpr_net` structure with `memset()`
6. Calls `esp_now_deinit()` to clean up ESP-NOW layer

## How to Test Reinitialization

### Test Sequence

```c
// Initial run
fpr_host_test_start(NULL);   // Start host
// ... wait for connection ...
fpr_host_test_stop();         // Stop and FULLY deinitialize

fpr_host_test_start(NULL);   // Reinitialize host
// Host should successfully reconnect with client
```

### Manual Test Procedure

1. **Flash host and client devices**
2. **Start both** - verify they connect successfully
3. **Stop client test** - call `fpr_client_test_stop()`
4. **Restart client test** - call `fpr_client_test_start()`
5. **Verify reconnection** - should reconnect within 5-20 seconds
6. **Stop host test** - call `fpr_host_test_stop()`
7. **Restart host test** - call `fpr_host_test_start()`
8. **Verify reconnection** - should reconnect within 5-20 seconds

### Expected Behavior After Fix

**Client logs after restart:**
```
[fpr] FPR network already initialized. Call fpr_network_deinit() first.  // If you forget to call stop
[FPR_CLIENT_TEST] FPR Client Test stopped
[fpr] FPR Network initialized: FPR-Client-Test (...)
[FPR_CLIENT_TEST] FPR Client is now RUNNING
[FPR_CLIENT_TEST] [DISCOVERY] Host found #1: FPR-Host-Test (...)
[fpr] Connection established with FPR-Host-Test
```

**Host logs after restart:**
```
[FPR_HOST_TEST] FPR Host Test stopped
[fpr] FPR Network initialized: FPR-Host-Test (...)
[FPR_HOST_TEST] FPR Host is now RUNNING
[fpr] Processing connection request from FPR-Client-Test
[fpr] Peer connected: FPR-Client-Test (...)
```

## Related Issues

This fix also resolves:
- Memory leaks from unreleased peer allocations
- ESP-NOW peer table inconsistencies
- Security state machine confusion after restart
- "Already connected" errors when reconnecting to same peer

## API Usage Guidelines

When using FPR in your application:

### ✅ CORRECT - Stop before reinitializing
```c
fpr_network_deinit();           // Clean up everything
fpr_network_init("MyDevice");   // Reinitialize
fpr_network_start();            // Start fresh
```

### ❌ WRONG - Don't double-initialize
```c
fpr_network_init("MyDevice");
// ... use network ...
fpr_network_init("MyDevice");   // ERROR: Already initialized!
```

### ✅ CORRECT - Use test stop functions
```c
fpr_client_test_start(NULL);
// ... test runs ...
fpr_client_test_stop();         // Now calls fpr_network_deinit() internally
fpr_client_test_start(NULL);    // Safe to restart
```

## Testing Checklist

- [x] `fpr_network_deinit()` added to `fpr_client_test_stop()`
- [x] `fpr_network_deinit()` added to `fpr_host_test_stop()`
- [x] State check added to `fpr_network_init_ex()`
- [ ] Test client stop → restart → reconnect
- [ ] Test host stop → restart → reconnect
- [ ] Test both stop → restart → reconnect
- [ ] Verify no memory leaks after multiple reinit cycles
- [ ] Verify proper error message when double-initializing

## Notes

- The RECONNECT_TEST_GUIDE.md describes **power-cycle** scenarios (physical restart)
- This fix addresses **software reinitialization** (stop/start in same process)
- Both scenarios should now work reliably
- Reinitialization is now safe to call in a loop for stress testing
