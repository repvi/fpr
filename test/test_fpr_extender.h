/**
 * FPR Extender Test API
 * 
 * Call these functions from your main.cpp to run the extender test.
 */

#ifndef TEST_FPR_EXTENDER_H
#define TEST_FPR_EXTENDER_H

#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start FPR extender test
 * 
 * This function initializes WiFi, FPR network, configures as extender,
 * and starts background tasks for relay monitoring and statistics.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_extender_test_start(void);

/**
 * @brief Stop the FPR extender test
 * 
 * Stops all test tasks and cleans up resources.
 */
void fpr_extender_test_stop(void);

/**
 * @brief Get extender test statistics
 * 
 * @param messages_relayed Output: number of messages relayed
 * @param bytes_relayed Output: total bytes relayed
 */
void fpr_extender_test_get_stats(uint32_t *messages_relayed, uint32_t *bytes_relayed);

#ifdef __cplusplus
}
#endif

#endif // TEST_FPR_EXTENDER_H
