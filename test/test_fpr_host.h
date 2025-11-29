/**
 * FPR Host Test API
 * 
 * Call these functions from your main.cpp to run the host test.
 */

#ifndef TEST_FPR_HOST_H
#define TEST_FPR_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for host test
 */
typedef struct {
    bool auto_mode;         // true for auto connection, false for manual
    uint32_t max_peers;     // Maximum peers (0 = unlimited)
    bool echo_enabled;      // Echo received data back to sender
} fpr_host_test_config_t;

/**
 * @brief Initialize and start FPR host test
 * 
 * This function initializes WiFi, FPR network, configures as host,
 * and starts background tasks for statistics and monitoring.
 * 
 * @param config Test configuration (NULL for defaults)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_host_test_start(const fpr_host_test_config_t *config);

/**
 * @brief Stop the FPR host test
 * 
 * Stops all test tasks and cleans up resources.
 */
void fpr_host_test_stop(void);

/**
 * @brief Get host test statistics
 * 
 * @param peers_discovered Output: number of peers discovered
 * @param peers_connected Output: number of peers connected
 * @param messages_received Output: number of messages received
 * @param bytes_received Output: total bytes received
 */
void fpr_host_test_get_stats(uint32_t *peers_discovered, uint32_t *peers_connected,
                              uint32_t *messages_received, uint32_t *bytes_received);

#ifdef __cplusplus
}
#endif

#endif // TEST_FPR_HOST_H
