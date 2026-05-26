# Using SPROUT from main.cpp

This guide explains how to use the SPROUT API directly in your application.

## Overview

SPROUT provides a simple, function-based API. Use the base functions directly - no wrapper functions are needed.

## Basic Usage

### Step 1: Include the SPROUT Header

```c
#include "sprout/sprout.h"
```

### Step 2: Initialize SPROUT

```c
esp_err_t ret = sprout_init("MyDeviceName");
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPROUT init failed: %s", esp_err_to_name(ret));
    return;
}
```

### Step 3: Register Callbacks

```c
static void on_data_received(void *peer_addr, void *data, void *user_data)
{
    ESP_LOGI(TAG, "Received data from peer");
}

sprout_register_receive_callback(on_data_received, NULL);
```

### Step 4: Set Mode and Start

```c
// Set mode (HOST, CLIENT, EXTENDER, or BROADCAST)
sprout_set_mode(SPROUT_MODE_HOST);

// Start the network
ret = sprout_start();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPROUT start failed: %s", esp_err_to_name(ret));
    return;
}
```

## Complete Example

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sprout/sprout.h"
#include "esp_log.h"

static const char *TAG = "MY_APP";

static void on_data_received(void *peer_addr, void *data, void *user_data)
{
    ESP_LOGI(TAG, "Received data from peer");
}

void app_main()
{
    ESP_LOGI(TAG, "Starting SPROUT");
    
    // Initialize
    esp_err_t ret = sprout_init("MyDevice");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register callback
    sprout_register_receive_callback(on_data_received, NULL);
    
    // Set mode
    sprout_set_mode(SPROUT_MODE_HOST);
    
    // Start
    ret = sprout_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "SPROUT running");
    
    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Example: broadcast message
        const char *msg = "Hello";
        sprout_broadcast((uint8_t *)msg, strlen(msg), SPROUT_PACKAGE_DATA);
    }
}
```

## Common Operations

### Send Data to Peer

```c
uint8_t peer_address[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
const char *data = "Hello peer";
sprout_send(peer_address, (void *)data, strlen(data), SPROUT_PACKAGE_DATA);
```

### Broadcast Data

```c
const char *data = "Hello everyone";
sprout_broadcast((uint8_t *)data, strlen(data), SPROUT_PACKAGE_DATA);
```

### Get Network State

```c
sprout_network_state_t state = sprout_get_state();
ESP_LOGI(TAG, "Network state: %d", state);
```

### Check if Running

```c
if (sprout_is_loop_task_running()) {
    ESP_LOGI(TAG, "Network loop task is running");
}
```

### Stop Network

```c
sprout_stop();
```

### Deinitialize

```c
sprout_deinit();
```

## Modes

- **SPROUT_MODE_HOST**: Accept connections from clients
- **SPROUT_MODE_CLIENT**: Connect to hosts
- **SPROUT_MODE_EXTENDER**: Relay messages in mesh network
- **SPROUT_MODE_BROADCAST**: Send-only mode

## See Also

- [API.md](../docs/API.md) - Complete API reference
- [README.md](../README.md) - Project overview
