#ifndef CD4051_SERVICE_H
#define CD4051_SERVICE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lib/adc_events.h"

#ifdef __cplusplus
extern "C" {
#endif

// only supports ADC1 channels (GPIOs 32-39)
typedef struct cd4051_t {
    portMUX_TYPE _lock;
    adc_events_handler_t _adc_event_handler;
    gpio_num_t _s0;
    gpio_num_t _s1;
    gpio_num_t _s2;
    gpio_num_t _inh;         // INH (inhibit) pin for enabling/disabling the chip
    int _inter;
    uint32_t _magic;         // Magic number for validation
    bool _initialized;       // Initialization flag
} cd4051_t;

/**
 * @brief Initialize CD4051 multiplexer
 * @param cd4051 Pointer to CD4051 structure
 * @param input ADC input GPIO pin (must be ADC1 channel: GPIO 32-39)
 * @param s0 S0 control pin
 * @param s1 S1 control pin
 * @param s2 S2 control pin
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_init(cd4051_t *cd4051, gpio_num_t input, gpio_num_t s0, gpio_num_t s1, gpio_num_t s2);

/**
 * @brief Read raw ADC value from specific channel
 * @param cd4051 Pointer to CD4051 structure
 * @param channel Channel number (0-7)
 * @return Raw ADC value (0-4095) or -1 on error
 */
int cd4051_read_channel_raw(cd4051_t *cd4051, uint8_t channel);

/**
 * @brief Read calibrated voltage from specific channel
 * @param cd4051 Pointer to CD4051 structure
 * @param channel Channel number (0-7)
 * @return Voltage in millivolts (mV) or -1 on error
 */
int cd4051_read_channel_voltage(cd4051_t *cd4051, uint8_t channel);

/**
 * @brief Enable ADC monitoring for a channel
 * @param cd4051 Pointer to CD4051 structure
 * @param channel Channel number (0-7)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_enable_channel(cd4051_t *cd4051, uint8_t channel);

/**
 * @brief Disable ADC monitoring for a channel
 * @param cd4051 Pointer to CD4051 structure
 * @param channel Channel number (0-7)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_disable_channel(cd4051_t *cd4051, uint8_t channel);

/**
 * @brief Read value from channel's queue (non-blocking with timeout)
 * @param cd4051 Pointer to CD4051 structure
 * @param channel Channel number (0-7)
 * @param timeout Maximum wait time in ticks (use portMAX_DELAY for blocking)
 * @return ADC value from queue or -1 on timeout/error
 */
int cd4051_read_queue(cd4051_t *cd4051, uint8_t channel, TickType_t timeout);

/**
 * @brief Destroy CD4051 instance and free resources
 * @param cd4051 Pointer to CD4051 structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_destroy(cd4051_t *cd4051);

/**
 * @brief Set INH (inhibit) pin for enabling/disabling the chip
 * @param cd4051 Pointer to CD4051 structure
 * @param inh_pin INH pin GPIO number, or GPIO_NUM_NC if not used
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_set_inhibit_pin(cd4051_t *cd4051, gpio_num_t inh_pin);

/**
 * @brief Enable CD4051 chip via INH pin (if configured)
 * @param cd4051 Pointer to CD4051 structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_enable(cd4051_t *cd4051);

/**
 * @brief Disable CD4051 chip via INH pin (if configured)
 * @param cd4051 Pointer to CD4051 structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_disable(cd4051_t *cd4051);

/**
 * @brief Read averaged raw ADC value from channel for stability
 * @param cd4051 Pointer to CD4051 structure
 * @param channel Channel number (0-7)
 * @param samples Number of samples to average (1-32)
 * @return Averaged raw ADC value or -1 on error
 */
int cd4051_read_channel_averaged(cd4051_t *cd4051, uint8_t channel, uint8_t samples);

/**
 * @brief Get statistics for a specific channel
 * @param cd4051 Pointer to CD4051 structure
 * @param channel Channel number (0-7)
 * @param stats Pointer to statistics structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t cd4051_get_statistics(cd4051_t *cd4051, uint8_t channel, adc_event_statistics_t *stats);

/**
 * @brief Check if CD4051 is properly initialized
 * @param cd4051 Pointer to CD4051 structure
 * @return true if initialized, false otherwise
 */
bool cd4051_is_initialized(cd4051_t *cd4051);

#ifdef __cplusplus
}
#endif

#endif // CD4051_SERVICE_H