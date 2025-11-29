#ifndef ADC_EVENTS_H
#define ADC_EVENTS_H

#include "esp_adc/adc_oneshot.h"
#include "hal/gpio_types.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef void (*adc_event_callback_t)(void *);
typedef void (*adc_event_execute_fn_t)(void *);

/**
 * @brief ADC event types for different trigger conditions
 */
typedef enum adc_event_type_t {
    ADC_EVENT_TYPE_IN_RANGE = 0,      ///< Trigger when value is within range (with hysteresis)
    ADC_EVENT_TYPE_OUT_OF_RANGE,      ///< Trigger when value is outside range (with hysteresis)
    ADC_EVENT_TYPE_QUEUE,             ///< Push all values to queue for buffered reading
    ADC_EVENT_TYPE_RISING_EDGE,       ///< Trigger on rising edge (crosses lower threshold upward)
    ADC_EVENT_TYPE_FALLING_EDGE,      ///< Trigger on falling edge (crosses upper threshold downward)
    ADC_EVENT_TYPE_CHANGE             ///< Trigger on any significant change (> hysteresis)
} adc_event_type_t;

/**
 * @brief Statistics for ADC virtual channel
 */
typedef struct adc_event_statistics_t {
    int32_t min_value;              ///< Minimum value observed
    int32_t max_value;              ///< Maximum value observed
    int64_t sum_value;              ///< Sum of all values (for average calculation)
    uint32_t sample_count;          ///< Number of samples processed
    uint32_t error_count;           ///< Number of read errors
    uint32_t trigger_count;         ///< Number of times callback was triggered
    uint32_t queue_overflow_count;  ///< Number of queue overflow events
    int32_t last_value;             ///< Last value read
} adc_event_statistics_t;

/**
 * @brief Configuration structure for attaching ADC event handlers
 */
typedef struct adc_event_attach_t {
    adc_event_callback_t err_cb;        ///< Callback for event trigger
    adc_event_execute_fn_t hardware_fn; ///< Optional hardware function to execute before reading
    void *err_cb_arg;                   ///< Argument for event callback
    void *hardware_fn_arg;              ///< Argument for hardware function
    const char *name;                   ///< Name for this virtual channel
    int lower_range;                    ///< Lower threshold value
    int upper_range;                    ///< Upper threshold value
} adc_event_attach_t;

struct adc_events_t;
typedef struct adc_events_t* adc_events_handler_t;

// Convenience macros for common event types
#define adc_events_attach_in_range(handler, event_attach) adc_events_attach(handler, event_attach, ADC_EVENT_TYPE_IN_RANGE)
#define adc_events_attach_out_of_range(handler, event_attach) adc_events_attach(handler, event_attach, ADC_EVENT_TYPE_OUT_OF_RANGE)
#define adc_events_attach_queue(handler, event_attach) adc_events_attach(handler, event_attach, ADC_EVENT_TYPE_QUEUE)
#define adc_events_attach_rising_edge(handler, event_attach) adc_events_attach(handler, event_attach, ADC_EVENT_TYPE_RISING_EDGE)
#define adc_events_attach_falling_edge(handler, event_attach) adc_events_attach(handler, event_attach, ADC_EVENT_TYPE_FALLING_EDGE)
#define adc_events_attach_change(handler, event_attach) adc_events_attach(handler, event_attach, ADC_EVENT_TYPE_CHANGE)

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Core Functions
// ============================================================================

/**
 * @brief Create ADC events handler
 * 
 * @param pin GPIO pin number (must be valid ADC pin: GPIO32-39)
 * @param virtual_channels Number of virtual channels to create (1-32)
 * @return Handler pointer on success, NULL on failure
 */
adc_events_handler_t adc_events_create(gpio_num_t pin, int virtual_channels);

/**
 * @brief Destroy ADC events handler and free all resources
 * 
 * Stops task, timer, frees queues, calibration, and memory.
 * 
 * @param handler ADC events handler
 * @return ESP_OK on success
 */
esp_err_t adc_events_destroy(adc_events_handler_t handler);

/**
 * @brief Start ADC sampling task
 * 
 * @param handler ADC events handler
 * @param interval Sampling interval in milliseconds (1-10000)
 * @return ESP_OK on success
 */
esp_err_t adc_events_start_task(adc_events_handler_t handler, int interval);

// ============================================================================
// Configuration Functions
// ============================================================================

/**
 * @brief Helper to set event attach configuration
 * 
 * @param data Pointer to attach configuration structure
 * @param name Name for virtual channel (can be NULL)
 * @param fn Callback function
 * @param arg Callback argument
 * @param lower_range Lower threshold
 * @param upper_range Upper threshold
 */
void adc_event_attach_set(adc_event_attach_t *data, const char *name, 
                          adc_event_callback_t fn, void *arg, 
                          int lower_range, int upper_range);

/**
 * @brief Attach event handler to virtual channel
 * 
 * @param handler ADC events handler
 * @param event_attach Event configuration
 * @param event_type Type of event trigger
 */
void adc_events_attach(adc_events_handler_t handler, 
                       const adc_event_attach_t *event_attach, 
                       adc_event_type_t event_type);

/**
 * @brief Update range for virtual channel at runtime
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 * @param lower New lower threshold
 * @param upper New upper threshold
 * @return ESP_OK on success
 */
esp_err_t adc_events_set_range(adc_events_handler_t handler, int index, 
                               int lower, int upper);

/**
 * @brief Set hysteresis for virtual channel
 * 
 * Hysteresis prevents jittery triggers near threshold boundaries.
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 * @param hysteresis Hysteresis value (default: 50)
 * @return ESP_OK on success
 */
esp_err_t adc_events_set_hysteresis(adc_events_handler_t handler, int index, 
                                    int hysteresis);

/**
 * @brief Configure moving average filter
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 * @param sample_count Number of samples to average (1-16)
 * @return ESP_OK on success
 */
esp_err_t adc_events_set_filter(adc_events_handler_t handler, int index, 
                                uint8_t sample_count);

/**
 * @brief Set error callback for ADC read failures
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 * @param error_cb Error callback function
 * @param arg Callback argument
 * @return ESP_OK on success
 */
esp_err_t adc_events_set_error_callback(adc_events_handler_t handler, int index,
                                        adc_event_callback_t error_cb, void *arg);

// ============================================================================
// Control Functions
// ============================================================================

/**
 * @brief Pause virtual channel (stop processing)
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 */
void adc_events_attached_pause(adc_events_handler_t handler, int index);

/**
 * @brief Resume virtual channel (continue processing)
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 */
void adc_events_attached_resume(adc_events_handler_t handler, int index);

// ============================================================================
// Statistics Functions
// ============================================================================

/**
 * @brief Get statistics for virtual channel
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 * @param stats Output statistics structure
 * @return ESP_OK on success
 */
esp_err_t adc_events_get_statistics(adc_events_handler_t handler, int index,
                                    adc_event_statistics_t *stats);

/**
 * @brief Reset statistics for virtual channel
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 * @return ESP_OK on success
 */
esp_err_t adc_events_reset_statistics(adc_events_handler_t handler, int index);

/**
 * @brief Get average value for virtual channel
 * 
 * @param handler ADC events handler
 * @param index Virtual channel index
 * @return Average value, or -1 on error
 */
int32_t adc_events_get_average(adc_events_handler_t handler, int index);

// ============================================================================
// Reading Functions
// ============================================================================

/**
 * @brief Read raw ADC value immediately (blocking)
 * 
 * @param handler ADC events handler
 * @return Raw ADC value, or -1 on error
 */
int adc_events_read_raw(adc_events_handler_t handler);

/**
 * @brief Read calibrated voltage immediately (blocking)
 * 
 * @param handler ADC events handler
 * @return Voltage in millivolts, or -1 on error
 */
int adc_events_read_voltage(adc_events_handler_t handler);

/**
 * @brief Get value from queue (for QUEUE type channels)
 * 
 * @param handler ADC events handler
 * @param await Timeout in FreeRTOS ticks (0 = no wait)
 * @param index Virtual channel index
 * @return Queued value, or -1 on timeout/error
 */
int adc_events_get_value_await(adc_events_handler_t handler, int await, int index);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Check if value is in range
 * 
 * @param val Value to check
 * @param lower_range Lower bound
 * @param upper_range Upper bound
 * @return true if in range
 */
bool adc_in_range(int val, int lower_range, int upper_range);

/**
 * @brief Check if value is out of range
 * 
 * @param val Value to check
 * @param lower_range Lower bound
 * @param upper_range Upper bound
 * @return true if out of range
 */
bool adc_out_of_range(int val, int lower_range, int upper_range);

/**
 * @brief Check if value is in range for virtual channel
 * 
 * @param handler ADC events handler
 * @param val Value to check
 * @param index Virtual channel index
 * @return true if in range
 */
bool adc_events_in_range(adc_events_handler_t handler, int val, int index);

/**
 * @brief Check if value is out of range for virtual channel
 * 
 * @param handler ADC events handler
 * @param val Value to check
 * @param index Virtual channel index
 * @return true if out of range
 */
bool adc_events_out_of_range(adc_events_handler_t handler, int val, int index);

/**
 * @brief Get number of attached virtual channels
 * 
 * @param handler ADC events handler
 * @return Number of attached channels
 */
int adc_events_attached_amount(adc_events_handler_t handler);

/**
 * @brief Get number of remaining virtual channels
 * 
 * @param handler ADC events handler
 * @return Number of available channels
 */
int adc_events_attached_remaining(adc_events_handler_t handler);

/**
 * @brief Check if handler is running
 * 
 * @param handler ADC events handler
 * @return true if running
 */
bool adc_events_is_running(adc_events_handler_t handler);

/**
 * @brief Print detailed information about handler and all channels
 * 
 * @param handler ADC events handler
 * @return ESP_OK on success
 */
esp_err_t adc_events_print_info(adc_events_handler_t handler);

#ifdef __cplusplus
}
#endif

#endif // ADC_EVENTS_H