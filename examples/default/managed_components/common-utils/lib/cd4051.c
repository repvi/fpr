#include "lib/cd4051.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_adc/adc_cali.h"
#include "esp_rom_sys.h"  // For esp_rom_delay_us()
#include "esp_log.h"
#include "string.h"

static const char *TAG = "CD4051";

#define CD4051_MAX_CHANNELS 8
#define CD4051_SETTLING_TIME_US 10  // CD4051 typical settling time
#define CD4051_DEFAULT_SAMPLE_INTERVAL_MS 50
#define CD4051_MIN_SAMPLE_INTERVAL_MS 10
#define CD4051_MAX_SAMPLE_INTERVAL_MS 1000
#define CD4051_MAGIC_NUMBER 0xCD4051A5  // For validation

static inline bool _cd4051_is_valid(cd4051_t *cd4051)
{
    return (cd4051 != NULL && cd4051->_magic == CD4051_MAGIC_NUMBER && cd4051->_adc_event_handler != NULL);
}

static inline void _cd4051_set_channel_pins(cd4051_t *cd4051, uint8_t channel)
{
    gpio_set_level(cd4051->_s0, (channel >> 0) & 0x01);
    gpio_set_level(cd4051->_s1, (channel >> 1) & 0x01);
    gpio_set_level(cd4051->_s2, (channel >> 2) & 0x01);
}

static void _cd4051_change_channel(void *arg)
{
    cd4051_t *cd4051 = (cd4051_t *)arg;
    if (!_cd4051_is_valid(cd4051)) {
        return;
    }
    
    uint8_t channel = cd4051->_inter;

    portENTER_CRITICAL(&cd4051->_lock);
    _cd4051_set_channel_pins(cd4051, channel);
    portEXIT_CRITICAL(&cd4051->_lock);

    esp_rom_delay_us(CD4051_SETTLING_TIME_US);

    int active_channels = adc_events_attached_amount(cd4051->_adc_event_handler);
    if (active_channels > 0) {
        cd4051->_inter = (cd4051->_inter + 1) % active_channels;
    }
}

esp_err_t cd4051_init(cd4051_t *cd4051, gpio_num_t input, gpio_num_t s0, gpio_num_t s1, gpio_num_t s2)
{
    if (cd4051 == NULL) {
        ESP_LOGE(TAG, "cd4051 pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate GPIO pins
    if (!GPIO_IS_VALID_OUTPUT_GPIO(s0) || !GPIO_IS_VALID_OUTPUT_GPIO(s1) || !GPIO_IS_VALID_OUTPUT_GPIO(s2)) {
        ESP_LOGE(TAG, "Invalid control GPIO pins");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Zero out structure for safety
    memset(cd4051, 0, sizeof(cd4051_t));
    
    cd4051->_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    cd4051->_s0 = s0;
    cd4051->_s1 = s1;
    cd4051->_s2 = s2;
    cd4051->_inh = GPIO_NUM_NC;  // Not connected by default
    cd4051->_inter = 0;
    cd4051->_initialized = false;

    cd4051->_adc_event_handler = adc_events_create(input, CD4051_MAX_CHANNELS);
    if (cd4051->_adc_event_handler == NULL) {
        ESP_LOGE(TAG, "Failed to create ADC event handler for pin %d", input);
        return ESP_FAIL;
    }

    for (int i = 0; i < CD4051_MAX_CHANNELS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "CD4051_CH%d", i);
        adc_event_attach_t event_attach = {
            .hardware_fn = _cd4051_change_channel,
            .hardware_fn_arg = cd4051,
            .name = name,
            .lower_range = 0,
            .upper_range = 4095
        };
        adc_events_attach(cd4051->_adc_event_handler, &event_attach, ADC_EVENT_TYPE_QUEUE);
        if (i >= 2) {
            adc_events_attached_pause(cd4051->_adc_event_handler, i); // Start with only first two channels active
        }
    }

    // Configure the CD4051 control pins as outputs
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s0) | (1ULL << s1) | (1ULL << s2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(err));
        adc_events_destroy(cd4051->_adc_event_handler);
        return err;
    }

    // Set initial channel to 0
    portENTER_CRITICAL(&cd4051->_lock);
    _cd4051_set_channel_pins(cd4051, 0);
    portEXIT_CRITICAL(&cd4051->_lock);

    err = adc_events_start_task(cd4051->_adc_event_handler, CD4051_DEFAULT_SAMPLE_INTERVAL_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ADC events task: %s", esp_err_to_name(err));
        adc_events_destroy(cd4051->_adc_event_handler);
        return err;
    }
    
    cd4051->_initialized = true;
    cd4051->_magic = CD4051_MAGIC_NUMBER;
    
    ESP_LOGI(TAG, "CD4051 initialized: input=%d, s0=%d, s1=%d, s2=%d", input, s0, s1, s2);
    return ESP_OK;
}

int cd4051_read_channel_raw(cd4051_t *cd4051, uint8_t channel)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return -1;
    }
    
    if (channel >= CD4051_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel %d (must be 0-%d)", channel, CD4051_MAX_CHANNELS - 1);
        return -1;
    }
    
    portENTER_CRITICAL(&cd4051->_lock);
    _cd4051_set_channel_pins(cd4051, channel);
    portEXIT_CRITICAL(&cd4051->_lock);

    esp_rom_delay_us(CD4051_SETTLING_TIME_US);

    int adc_reading = adc_events_read_raw(cd4051->_adc_event_handler);
    if (adc_reading < 0) {
        ESP_LOGE(TAG, "Failed to read ADC for channel %d", channel);
    }
    
    return adc_reading;
}

int cd4051_read_channel_voltage(cd4051_t *cd4051, uint8_t channel)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return -1;
    }
    
    if (channel >= CD4051_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel %d (must be 0-%d)", channel, CD4051_MAX_CHANNELS - 1);
        return -1;
    }
    
    portENTER_CRITICAL(&cd4051->_lock);
    _cd4051_set_channel_pins(cd4051, channel);
    portEXIT_CRITICAL(&cd4051->_lock);

    esp_rom_delay_us(CD4051_SETTLING_TIME_US);

    int voltage_mv = adc_events_read_voltage(cd4051->_adc_event_handler);
    if (voltage_mv < 0) {
        ESP_LOGE(TAG, "Failed to read voltage for channel %d", channel);
    }
    
    return voltage_mv;  // Returns millivolts (mV)
}

esp_err_t cd4051_enable_channel(cd4051_t *cd4051, uint8_t channel)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (channel >= CD4051_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel %d", channel);
        return ESP_ERR_INVALID_ARG;
    }
    
    adc_events_attached_resume(cd4051->_adc_event_handler, channel);
    ESP_LOGI(TAG, "Enabled channel %d", channel);
    return ESP_OK;
}

esp_err_t cd4051_disable_channel(cd4051_t *cd4051, uint8_t channel)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (channel >= CD4051_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel %d", channel);
        return ESP_ERR_INVALID_ARG;
    }
    
    adc_events_attached_pause(cd4051->_adc_event_handler, channel);
    ESP_LOGI(TAG, "Disabled channel %d", channel);
    return ESP_OK;
}

int cd4051_read_queue(cd4051_t *cd4051, uint8_t channel, TickType_t timeout)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return -1;
    }
    
    if (channel >= CD4051_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel %d (must be 0-%d)", channel, CD4051_MAX_CHANNELS - 1);
        return -1;
    }
    
    return adc_events_get_value_await(cd4051->_adc_event_handler, timeout, channel);
}

esp_err_t cd4051_destroy(cd4051_t *cd4051)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Destroying CD4051...");
    
    cd4051->_initialized = false;
    cd4051->_magic = 0;  // Invalidate immediately
    
    esp_err_t err = adc_events_destroy(cd4051->_adc_event_handler);
    cd4051->_adc_event_handler = NULL;
    
    // Reset GPIO pins to safe state
    gpio_set_level(cd4051->_s0, 0);
    gpio_set_level(cd4051->_s1, 0);
    gpio_set_level(cd4051->_s2, 0);
    
    if (cd4051->_inh != GPIO_NUM_NC) {
        gpio_set_level(cd4051->_inh, 1);  // Disable the chip
    }
    
    return err;
}

esp_err_t cd4051_set_inhibit_pin(cd4051_t *cd4051, gpio_num_t inh_pin)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (inh_pin != GPIO_NUM_NC && !GPIO_IS_VALID_OUTPUT_GPIO(inh_pin)) {
        ESP_LOGE(TAG, "Invalid INH pin: %d", inh_pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (inh_pin != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << inh_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure INH pin: %s", esp_err_to_name(err));
            return err;
        }
        gpio_set_level(inh_pin, 0);  // Enable the chip (INH is active high)
    }
    
    cd4051->_inh = inh_pin;
    ESP_LOGI(TAG, "INH pin set to %d", inh_pin);
    return ESP_OK;
}

esp_err_t cd4051_enable(cd4051_t *cd4051)
{
    if (!_cd4051_is_valid(cd4051)) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (cd4051->_inh != GPIO_NUM_NC) {
        gpio_set_level(cd4051->_inh, 0);  // Enable (INH low)
        ESP_LOGI(TAG, "CD4051 enabled");
    }
    return ESP_OK;
}

esp_err_t cd4051_disable(cd4051_t *cd4051)
{
    if (!_cd4051_is_valid(cd4051)) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (cd4051->_inh != GPIO_NUM_NC) {
        gpio_set_level(cd4051->_inh, 1);  // Disable (INH high)
        ESP_LOGI(TAG, "CD4051 disabled");
    }
    return ESP_OK;
}

int cd4051_read_channel_averaged(cd4051_t *cd4051, uint8_t channel, uint8_t samples)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return -1;
    }
    
    if (channel >= CD4051_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel %d", channel);
        return -1;
    }
    
    if (samples == 0 || samples > 32) {
        ESP_LOGE(TAG, "Invalid sample count %d (must be 1-32)", samples);
        return -1;
    }
    
    int64_t sum = 0;
    uint8_t valid_samples = 0;
    
    for (uint8_t i = 0; i < samples; i++) {
        int reading = cd4051_read_channel_raw(cd4051, channel);
        if (reading >= 0) {
            sum += reading;
            valid_samples++;
        }
        
        if (i < samples - 1) {
            vTaskDelay(pdMS_TO_TICKS(2));  // Small delay between samples
        }
    }
    
    if (valid_samples == 0) {
        ESP_LOGE(TAG, "No valid samples read from channel %d", channel);
        return -1;
    }
    
    return (int)(sum / valid_samples);
}

esp_err_t cd4051_get_statistics(cd4051_t *cd4051, uint8_t channel, adc_event_statistics_t *stats)
{
    if (!_cd4051_is_valid(cd4051)) {
        ESP_LOGE(TAG, "Invalid CD4051 instance");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (channel >= CD4051_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid channel %d", channel);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (stats == NULL) {
        ESP_LOGE(TAG, "Statistics pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    return adc_events_get_statistics(cd4051->_adc_event_handler, channel, stats);
}

bool cd4051_is_initialized(cd4051_t *cd4051)
{
    return _cd4051_is_valid(cd4051) && cd4051->_initialized;
}