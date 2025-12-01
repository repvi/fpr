/**
 * @file test_fpr_data_sizes.h
 * @brief FPR Data Size Test API
 * 
 * Tests FPR library with various data payload sizes to verify
 * fragmentation and reassembly logic.
 */

#ifndef TEST_FPR_DATA_SIZES_H
#define TEST_FPR_DATA_SIZES_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for data size test
 */
typedef struct {
    bool auto_mode;              // true for auto connection, false for manual
    uint32_t test_interval_ms;   // Interval between test transmissions (default: 2000ms)
    bool echo_mode;              // Host echoes received data back to client
} fpr_data_size_test_config_t;

/**
 * @brief Start FPR data size test as HOST
 * 
 * @param config Test configuration (NULL for defaults)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_data_size_test_host_start(const fpr_data_size_test_config_t *config);

/**
 * @brief Start FPR data size test as CLIENT
 * 
 * @param config Test configuration (NULL for defaults)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_data_size_test_client_start(const fpr_data_size_test_config_t *config);

/**
 * @brief Stop the data size test (host or client)
 */
void fpr_data_size_test_stop(void);

/**
 * @brief Get test statistics
 * 
 * @param tests_passed Output: number of successful transfers
 * @param tests_failed Output: number of failed transfers
 * @param bytes_sent Output: total bytes sent
 * @param bytes_received Output: total bytes received
 */
void fpr_data_size_test_get_stats(uint32_t *tests_passed, uint32_t *tests_failed,
                                   uint32_t *bytes_sent, uint32_t *bytes_received);

#ifdef __cplusplus
}
#endif

#endif // TEST_FPR_DATA_SIZES_H
