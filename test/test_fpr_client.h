/**
 * FPR Client Test API
 * 
 * Call these functions from your main.cpp to run the client test.
 */

#ifndef TEST_FPR_CLIENT_H
#define TEST_FPR_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for client test
 */
typedef struct {
    bool auto_mode;                 // true for auto connection, false for manual
    uint32_t scan_duration_ms;      // How long to scan for hosts (manual mode)
    uint32_t message_interval_ms;   // How often to send test messages
    bool use_latest_only_mode;      // Use FPR_QUEUE_MODE_LATEST_ONLY (real-time mode)
} fpr_client_test_config_t;

/**
 * @brief Initialize and start FPR client test
 * 
 * This function initializes WiFi, FPR network, configures as client,
 * and starts background tasks for connection, messaging, and statistics.
 * 
 * @param config Test configuration (NULL for defaults)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t fpr_client_test_start(const fpr_client_test_config_t *config);

/**
 * @brief Stop the FPR client test
 * 
 * Stops all test tasks and cleans up resources.
 */
void fpr_client_test_stop(void);

/**
 * @brief Get client test statistics
 * 
 * @param is_connected Output: true if connected to a host
 * @param hosts_found Output: number of hosts discovered
 * @param messages_sent Output: number of messages sent
 * @param messages_received Output: number of messages received
 */
void fpr_client_test_get_stats(bool *is_connected, uint32_t *hosts_found,
                                uint32_t *messages_sent, uint32_t *messages_received);

/**
 * @brief Check if client is connected
 * 
 * @return true if connected to a host, false otherwise
 */
bool fpr_client_test_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // TEST_FPR_CLIENT_H
