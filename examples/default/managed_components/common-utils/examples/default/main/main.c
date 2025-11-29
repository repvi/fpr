#include "lib/adc_events.h"
#include "standard/time.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ADC_TEST";

// Callback function for ADC events
void adc_value_changed_callback(void *arg)
{
    int *counter = (int *)arg;
    (*counter)++;
    ESP_LOGI(TAG, "ADC value changed! Trigger count: %d", *counter);
}

void adc_in_range_callback(void *arg)
{
    ESP_LOGI(TAG, "ADC value is in range!");
}

void adc_out_of_range_callback(void *arg)
{
    ESP_LOGI(TAG, "ADC value is out of range!");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ADC test on GPIO36 (ADC1_CH0)");
    ESP_LOGI(TAG, "This pin is safe to use and doesn't interfere with WiFi");
    
    // Create ADC handler with 3 virtual channels
    // GPIO36 is ADC1_CH0 and is WiFi-safe
    adc_events_handler_t adc_handler = adc_events_create(GPIO_NUM_36, 3);
    
    if (adc_handler == NULL) {
        ESP_LOGE(TAG, "Failed to create ADC handler!");
        return;
    }
    
    // Test immediate reading
    ESP_LOGI(TAG, "\n=== Testing immediate ADC reading ===");
    int raw_value = adc_events_read_raw(adc_handler);
    int voltage = adc_events_read_voltage(adc_handler);
    ESP_LOGI(TAG, "Raw ADC value: %d", raw_value);
    ESP_LOGI(TAG, "Calibrated voltage: %d mV", voltage);
    
    // Setup virtual channel 0: Monitor value changes
    static int change_counter = 0;
    adc_event_attach_t change_attach;
    adc_event_attach_set(&change_attach, "Change Monitor", 
                        adc_value_changed_callback, &change_counter, 
                        0, 4095);  // Full range
    adc_events_attach_change(adc_handler, &change_attach);
    adc_events_set_hysteresis(adc_handler, 0, 100);  // Trigger on 100+ change
    
    // Setup virtual channel 1: In-range detection (mid-range)
    adc_event_attach_t in_range_attach;
    adc_event_attach_set(&in_range_attach, "Mid-Range Detector", 
                        adc_in_range_callback, NULL, 
                        1500, 2500);  // Detect values between 1500-2500
    adc_events_attach_in_range(adc_handler, &in_range_attach);
    
    // Setup virtual channel 2: Out-of-range detection
    adc_event_attach_t out_range_attach;
    adc_event_attach_set(&out_range_attach, "Out-of-Range Detector", 
                        adc_out_of_range_callback, NULL, 
                        1800, 2200);  // Detect values outside 1800-2200
    adc_events_attach_out_of_range(adc_handler, &out_range_attach);
    
    // Start sampling at 100ms intervals
    esp_err_t err = adc_events_start_task(adc_handler, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ADC task!");
        adc_events_destroy(adc_handler);
        return;
    }
    
    ESP_LOGI(TAG, "\n=== ADC monitoring started ===");
    ESP_LOGI(TAG, "Sampling GPIO36 every 100ms");
    ESP_LOGI(TAG, "Connect a voltage source (0-3.3V) to GPIO36 to test");
    ESP_LOGI(TAG, "Try connecting to GND (0V) or 3.3V to trigger events\n");
    
    // Monitor and print statistics every 5 seconds
    for (int i = 0; i < 12; i++) {  // Run for 1 minute
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        ESP_LOGI(TAG, "\n=== Statistics Update (%.0f seconds) ===", (i + 1) * 5.0);
        
        // Read current value
        int current_raw = adc_events_read_raw(adc_handler);
        int current_voltage = adc_events_read_voltage(adc_handler);
        ESP_LOGI(TAG, "Current: raw=%d, voltage=%d mV", current_raw, current_voltage);
        
        // Print statistics for each channel
        for (int ch = 0; ch < 3; ch++) {
            adc_event_statistics_t stats;
            if (adc_events_get_statistics(adc_handler, ch, &stats) == ESP_OK) {
                int32_t avg = adc_events_get_average(adc_handler, ch);
                ESP_LOGI(TAG, "Channel %d: samples=%lu, triggers=%lu, avg=%ld, min=%ld, max=%ld",
                         ch, stats.sample_count, stats.trigger_count, 
                         avg, stats.min_value, stats.max_value);
            }
        }
        
        // Print detailed info every 20 seconds
        if ((i + 1) % 4 == 0) {
            adc_events_print_info(adc_handler);
        }
    }
    
    ESP_LOGI(TAG, "\n=== Test Complete ===");
    ESP_LOGI(TAG, "Cleaning up...");
    
    // Cleanup
    adc_events_destroy(adc_handler);
    
    ESP_LOGI(TAG, "ADC test finished");
}