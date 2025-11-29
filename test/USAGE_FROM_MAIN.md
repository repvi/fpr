# Using FPR Tests from main.cpp

The FPR test libraries are now callable from your `main.cpp` file. Include the appropriate header and call the test start function.

## Example: Host Test

```cpp
#include "fpr.h"
#include "test_fpr_host.h"

extern "C" void app_main(void)
{
    // Configure host test
    fpr_host_test_config_t config = {
        .auto_mode = true,      // Auto-approve connections
        .max_peers = 5,         // Max 5 peers
        .echo_enabled = true    // Echo data back
    };
    
    // Start the test
    esp_err_t err = fpr_host_test_start(&config);
    if (err != ESP_OK) {
        printf("Host test failed to start: %s\n", esp_err_to_name(err));
        return;
    }
    
    // Test runs in background tasks
    // You can add your own code here
    
    // Optional: Get statistics
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        
        uint32_t disc, conn, msgs, bytes;
        fpr_host_test_get_stats(&disc, &conn, &msgs, &bytes);
        printf("Host stats - Discovered: %lu, Connected: %lu, Messages: %lu\n",
               disc, conn, msgs);
    }
}
```

## Example: Client Test

```cpp
#include "fpr.h"
#include "test_fpr_client.h"

extern "C" void app_main(void)
{
    // Configure client test
    fpr_client_test_config_t config = {
        .auto_mode = true,               // Auto-connect to first host
        .scan_duration_ms = 5000,        // Scan for 5 seconds (manual mode)
        .message_interval_ms = 5000      // Send message every 5 seconds
    };
    
    // Start the test
    esp_err_t err = fpr_client_test_start(&config);
    if (err != ESP_OK) {
        printf("Client test failed to start: %s\n", esp_err_to_name(err));
        return;
    }
    
    // Test runs in background tasks
    // You can add your own code here
    
    // Optional: Monitor connection
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        
        if (fpr_client_test_is_connected()) {
            printf("Client is connected!\n");
        } else {
            printf("Client is not connected.\n");
        }
    }
}
```

## Example: Extender Test

```cpp
#include "fpr.h"
#include "test_fpr_extender.h"

extern "C" void app_main(void)
{
    // Start extender test (no configuration needed)
    esp_err_t err = fpr_extender_test_start();
    if (err != ESP_OK) {
        printf("Extender test failed to start: %s\n", esp_err_to_name(err));
        return;
    }
    
    printf("Extender running - relaying messages\n");
    
    // Test runs in background tasks
    // You can add your own code here
    
    // Optional: Get relay statistics
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        
        uint32_t msgs, bytes;
        fpr_extender_test_get_stats(&msgs, &bytes);
        printf("Relayed %lu messages, %lu bytes total\n", msgs, bytes);
    }
}
```

## Example: Running Multiple Tests (Advanced)

You can also run different tests based on a button press or configuration:

```cpp
#include "fpr.h"
#include "test_fpr_host.h"
#include "test_fpr_client.h"
#include "test_fpr_extender.h"

typedef enum {
    MODE_HOST,
    MODE_CLIENT,
    MODE_EXTENDER
} test_mode_t;

extern "C" void app_main(void)
{
    // Determine mode (e.g., from GPIO, NVS, or compile-time define)
    test_mode_t mode = MODE_HOST;  // Change as needed
    
    switch (mode) {
        case MODE_HOST: {
            fpr_host_test_config_t config = {
                .auto_mode = true,
                .max_peers = 10,
                .echo_enabled = true
            };
            fpr_host_test_start(&config);
            printf("Running as HOST\n");
            break;
        }
        
        case MODE_CLIENT: {
            fpr_client_test_config_t config = {
                .auto_mode = true,
                .scan_duration_ms = 5000,
                .message_interval_ms = 3000
            };
            fpr_client_test_start(&config);
            printf("Running as CLIENT\n");
            break;
        }
        
        case MODE_EXTENDER: {
            fpr_extender_test_start();
            printf("Running as EXTENDER\n");
            break;
        }
    }
    
    // Your application code continues here
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        printf("Application running...\n");
    }
}
```

## Stopping Tests

If you need to stop a test (e.g., to switch modes):

```cpp
// Stop host test
fpr_host_test_stop();

// Stop client test
fpr_client_test_stop();

// Stop extender test
fpr_extender_test_stop();
```

## Build Instructions

1. Edit your `main/main.cpp` with one of the examples above
2. Build normally:
   ```bash
   idf.py build
   ```
3. Flash and monitor:
   ```bash
   idf.py flash monitor
   ```

## Notes

- All test functions are non-blocking and run in background FreeRTOS tasks
- You can call these functions alongside your own application code
- The test library automatically initializes WiFi, NVS, and FPR
- Multiple tests should not be run simultaneously (they will conflict)
- Each test creates its own FreeRTOS tasks for statistics, monitoring, etc.
- Configuration is optional - passing NULL uses default values

## Header Files

Include these headers in your main.cpp:

```cpp
#include "test_fpr_host.h"      // For host test functions
#include "test_fpr_client.h"    // For client test functions
#include "test_fpr_extender.h"  // For extender test functions
```

All headers are located in: `components/fpr/test/`
