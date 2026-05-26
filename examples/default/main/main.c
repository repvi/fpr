#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sprout/sprout.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

static const char *TAG = "SPROUT_DEFAULT_MAIN";

// Example data receive callback
static void on_data_received(void *peer_addr, void *data, void *user_data)
{
    ESP_LOGI(TAG, "Received data from peer");
}

void app_main()
{
    ESP_LOGI(TAG, "Starting SPROUT example");
    
    // Initialize SPROUT
    esp_err_t ret = sprout_init("MyDevice");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPROUT init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register callback
    sprout_register_receive_callback(on_data_received);
    
    // Set mode (example: HOST mode)
    sprout_set_mode(SPROUT_MODE_HOST);
    
    // Start the network
    ret = sprout_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPROUT start failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "SPROUT network started successfully");
    
    // Main loop - keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Example: broadcast a message periodically
        // const char *msg = "Hello from SPROUT";
        // sprout_broadcast((uint8_t *)msg, strlen(msg), SPROUT_PACKAGE_DATA);
    }
}