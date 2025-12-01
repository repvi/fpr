#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fpr/fpr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

static const char *TAG = "FPR_DEFAULT_MAIN";

#ifdef CONFIG_FPR_TEST_HOST
#define FPR_TEST_HOST CONFIG_FPR_TEST_HOST
#endif
#ifdef CONFIG_FPR_TEST_CLIENT
#define FPR_TEST_CLIENT CONFIG_FPR_TEST_CLIENT
#endif
#ifdef CONFIG_FPR_TEST_EXTENDER
#define FPR_TEST_EXTENDER CONFIG_FPR_TEST_EXTENDER
#endif
#ifdef CONFIG_FPR_TEST_AUTO_START
#define FPR_TEST_AUTO_START CONFIG_FPR_TEST_AUTO_START
#endif
#ifdef CONFIG_FPR_TEST_DATA_SIZES
#define FPR_TEST_DATA_SIZES CONFIG_FPR_TEST_DATA_SIZES
#endif
/*
 * Test selection macros (choose one):
 * - Define `FPR_TEST_HOST` to build the host test into main
 * - Define `FPR_TEST_CLIENT` to build the client test into main
 * - Define `FPR_TEST_EXTENDER` to build the extender test into main
 * - Define `FPR_TEST_DATA_SIZES` to build the data size test into main
 *
 * Optionally define `FPR_TEST_AUTO_START` to automatically start the
 * selected test from `app_main()` with sane defaults.
 *
 * Define these via your build system. Example (in `main/CMakeLists.txt`):
 *   target_compile_definitions(${COMPONENT_LIB} PRIVATE FPR_TEST_HOST)
 */

#if defined(FPR_TEST_HOST) && (defined(FPR_TEST_CLIENT) || defined(FPR_TEST_EXTENDER) || defined(FPR_TEST_DATA_SIZES))
#error "Define only one of FPR_TEST_HOST, FPR_TEST_CLIENT, FPR_TEST_EXTENDER, FPR_TEST_DATA_SIZES"
#endif
#if defined(FPR_TEST_CLIENT) && (defined(FPR_TEST_EXTENDER) || defined(FPR_TEST_DATA_SIZES))
#error "Define only one of FPR_TEST_HOST, FPR_TEST_CLIENT, FPR_TEST_EXTENDER, FPR_TEST_DATA_SIZES"
#endif
#if defined(FPR_TEST_EXTENDER) && defined(FPR_TEST_DATA_SIZES)
#error "Define only one of FPR_TEST_HOST, FPR_TEST_CLIENT, FPR_TEST_EXTENDER, FPR_TEST_DATA_SIZES"
#endif

#if defined(FPR_TEST_HOST)
#include "test_fpr_host.h"
#elif defined(FPR_TEST_CLIENT)
#include "test_fpr_client.h"

#elif defined(FPR_TEST_EXTENDER)
#include "test_fpr_extender.h"
#elif defined(FPR_TEST_DATA_SIZES)
#include "test_fpr_data_sizes.h"
#endif

void app_main()
{
#if defined(FPR_TEST_HOST)
#ifdef FPR_TEST_AUTO_START
    {
        fpr_host_test_config_t cfg = {
            .auto_mode = false,
            .max_peers = 2,
            .echo_enabled = false
        };
        esp_err_t _err = fpr_host_test_start(&cfg);
        if (_err != ESP_OK) {
            ESP_LOGE(TAG, "fpr_host_test_start failed: %d", _err);
        } else {
            ESP_LOGI(TAG, "FPR host test started (AUTO)");
        }
    }
#else
    ESP_LOGI(TAG, "FPR host test compiled in; define FPR_TEST_AUTO_START to auto-start");
#endif
#elif defined(FPR_TEST_CLIENT)
#ifdef FPR_TEST_AUTO_START
    {
        fpr_client_test_config_t cfg = {
            .auto_mode = false,
            .scan_duration_ms = 5000,
            .message_interval_ms = 1000
        };
        esp_err_t _err = fpr_client_test_start(&cfg);
        if (_err != ESP_OK) {
            ESP_LOGE(TAG, "fpr_client_test_start failed: %d", _err);
        } else {
            ESP_LOGI(TAG, "FPR client test started (AUTO)");
        }
    }
#else
    ESP_LOGI(TAG, "FPR client test compiled in; define FPR_TEST_AUTO_START to auto-start");
#endif
#elif defined(FPR_TEST_EXTENDER)
#ifdef FPR_TEST_AUTO_START
    {
        esp_err_t _err = fpr_extender_test_start();
        if (_err != ESP_OK) {
            ESP_LOGE(TAG, "fpr_extender_test_start failed: %d", _err);
        } else {
            ESP_LOGI(TAG, "FPR extender test started (AUTO)");
        }
    }
#else
    ESP_LOGI(TAG, "FPR extender test compiled in; define FPR_TEST_AUTO_START to auto-start");
#endif
#elif defined(FPR_TEST_DATA_SIZES)
#ifdef FPR_TEST_AUTO_START
    // Use Kconfig settings for host/client mode
    #ifdef CONFIG_FPR_DATA_SIZE_TEST_HOST
    {
        // NULL config uses Kconfig defaults
        esp_err_t _err = fpr_data_size_test_host_start(NULL);
        if (_err != ESP_OK) {
            ESP_LOGE(TAG, "fpr_data_size_test_host_start failed: %d", _err);
        } else {
            ESP_LOGI(TAG, "FPR data size test started as HOST (Kconfig)");
        }
    }
    #else
    {
        // NULL config uses Kconfig defaults
        esp_err_t _err = fpr_data_size_test_client_start(NULL);
        if (_err != ESP_OK) {
            ESP_LOGE(TAG, "fpr_data_size_test_client_start failed: %d", _err);
        } else {
            ESP_LOGI(TAG, "FPR data size test started as CLIENT (Kconfig)");
        }
    }
    #endif
#else
    ESP_LOGI(TAG, "FPR data size test compiled in; define FPR_TEST_AUTO_START to auto-start");
#endif
#endif
}