/**
 * @file fpr_compat.c
 * @brief Legacy global-singleton API — thin wrappers over a default node.
 *
 * Every fpr_network_* / fpr_client_* / fpr_host_* / fpr_get_* function
 * forwards to the fpr_node_* API on `fpr_net`, the statically-allocated
 * default node. New code should use fpr_node.h instead; these wrappers
 * exist so existing applications keep working unchanged.
 *
 * `fpr_net` is THE documented legacy-API-only global: the old API has
 * no context parameter, so a process-wide node is inherent. Its
 * peers_lock is deliberately never deleted (ESP-NOW callback drain is
 * not guaranteed by IDF — a straggler may still reach for the lock).
 */

#include "fpr/fpr.h"
#include "fpr/fpr_node.h"
#include "fpr/fpr_config.h"
#include "fpr/internal/private_defs.h"
#include "fpr/internal/rx.h"
#include "esp_log.h"

/* Default node storage. `owns_storage` stays false: peers_lock is
 * process-lifetime and the struct is static — see file header. */
fpr_network_t fpr_net = {0};

static const char *TAG = "fpr_compat";

/* ticks -> ms with portMAX_DELAY preserved as "forever". */
static inline uint32_t _ticks_to_ms(TickType_t ticks)
{
    return (ticks == portMAX_DELAY) ? UINT32_MAX
                                    : (uint32_t)ticks * portTICK_PERIOD_MS;
}

// ========== LIFECYCLE ==========

esp_err_t fpr_network_init(const char *name)
{
    fpr_init_config_t config = {
        .channel = FPR_WIFI_CHANNEL,
        .power_mode = (fpr_power_mode_t)FPR_DEFAULT_POWER_MODE,
        .queue_mode = (fpr_queue_mode_t)FPR_DEFAULT_QUEUE_MODE,
        .transport = NULL,
    };
    return fpr_network_init_ex(name, &config);
}

esp_err_t fpr_network_init_ex(const char *name, const fpr_init_config_t *config)
{
    return fpr_node_init_at(&fpr_net, name, config);
}

esp_err_t fpr_network_deinit(void)
{
    return fpr_node_deinit(&fpr_net);
}

esp_err_t fpr_network_start(void)
{
    /* Legacy behaviour: start() always (re)installed the client-mode
     * handler. The node API drops that wart — restore it here. */
    if (fpr_net.state == FPR_STATE_INITIALIZED || fpr_net.state == FPR_STATE_STOPPED) {
        fpr_node_set_mode(&fpr_net, FPR_MODE_CLIENT);
    }
    return fpr_node_start(&fpr_net);
}

esp_err_t fpr_network_stop(void)
{
    return fpr_node_stop(&fpr_net);
}

esp_err_t fpr_network_pause(void)
{
    return fpr_node_pause(&fpr_net);
}

esp_err_t fpr_network_resume(void)
{
    return fpr_node_resume(&fpr_net);
}

fpr_network_state_t fpr_network_get_state(void)
{
    return fpr_node_get_state(&fpr_net);
}

// ========== MODE / STATE ACCESSORS ==========

void fpr_network_set_mode(fpr_mode_type_t mode)
{
    fpr_node_set_mode(&fpr_net, mode);
}

fpr_mode_type_t fpr_network_get_mode(void)
{
    return fpr_node_get_mode(&fpr_net);
}

void fpr_network_set_power_mode(fpr_power_mode_t mode)
{
    fpr_node_set_power_mode(&fpr_net, mode);
}

fpr_power_mode_t fpr_network_get_power_mode(void)
{
    return fpr_node_get_power_mode(&fpr_net);
}

uint8_t fpr_network_get_channel(void)
{
    return fpr_node_get_channel(&fpr_net);
}

void fpr_network_set_queue_mode(fpr_queue_mode_t mode)
{
    fpr_node_set_queue_mode(&fpr_net, mode);
}

fpr_queue_mode_t fpr_network_get_queue_mode(void)
{
    return fpr_node_get_queue_mode(&fpr_net);
}

esp_err_t fpr_network_set_peer_queue_mode(const uint8_t *peer_mac, fpr_queue_mode_t mode)
{
    return fpr_node_set_peer_queue_mode(&fpr_net, peer_mac, mode);
}

uint32_t fpr_network_get_peer_queued_packets(const uint8_t *peer_mac)
{
    return fpr_node_get_peer_queued_packets(&fpr_net, peer_mac);
}

void fpr_network_set_permission_state(fpr_visibility_t state)
{
    fpr_node_set_permission_state(&fpr_net, state);
}

fpr_visibility_t fpr_network_get_permission_state(void)
{
    return fpr_node_get_permission_state(&fpr_net);
}

// ========== PEERS ==========

esp_err_t fpr_network_add_peer(const uint8_t *peer_mac)
{
    return fpr_node_add_peer(&fpr_net, peer_mac);
}

esp_err_t fpr_network_remove_peer(const uint8_t *peer_mac)
{
    return fpr_node_remove_peer(&fpr_net, peer_mac);
}

int fpr_network_get_peer_count(void)
{
    return fpr_node_peer_count(&fpr_net);
}

size_t fpr_network_get_peers(fpr_data_receive_cb_t callback, void *user_data)
{
    return fpr_node_get_peers(&fpr_net, callback, user_data);
}

esp_err_t fpr_get_peer_info(const uint8_t *peer_mac, fpr_peer_info_t *info)
{
    return fpr_node_get_peer_info(&fpr_net, peer_mac, info);
}

size_t fpr_list_all_peers(fpr_peer_info_t *peer_array, size_t max_peers)
{
    return fpr_node_list_peers(&fpr_net, peer_array, max_peers);
}

esp_err_t fpr_get_peer_by_name(const char *peer_name, uint8_t *mac_out)
{
    return fpr_node_get_peer_by_name(&fpr_net, peer_name, mac_out);
}

esp_err_t fpr_clear_all_peers(void)
{
    return fpr_node_clear_peers(&fpr_net);
}

bool fpr_is_peer_reachable(const uint8_t *peer_mac, uint32_t timeout_ms)
{
    return fpr_node_is_peer_reachable(&fpr_net, peer_mac, timeout_ms);
}

// ========== SEND ==========

esp_err_t fpr_send_with_options(const uint8_t *peer_address, const void *data, int size, const fpr_send_options_t *options)
{
    return fpr_node_send_with_options(&fpr_net, peer_address, data, size, options);
}

esp_err_t fpr_network_send_to_peer(const uint8_t *peer_address, const void *data, int size, fpr_package_id_t package_id)
{
    return fpr_node_send(&fpr_net, peer_address, data, size, package_id);
}

esp_err_t fpr_network_broadcast(const void *data, int size, fpr_package_id_t package_id)
{
    return fpr_node_broadcast(&fpr_net, data, size, package_id);
}

esp_err_t fpr_network_send_device_info(const uint8_t *peer_address)
{
    return fpr_node_send_device_info(&fpr_net, peer_address);
}

esp_err_t fpr_network_broadcast_device_info(void)
{
    return fpr_node_broadcast_device_info(&fpr_net);
}

bool fpr_network_get_data_from_peer(const uint8_t *peer_mac, void *data, int data_size, TickType_t timeout)
{
    return fpr_node_get_data_from_peer(&fpr_net, peer_mac, data, data_size,
                                       _ticks_to_ms(timeout));
}

// ========== CALLBACK ==========

void fpr_register_receive_callback(fpr_data_receive_cb_t callback)
{
    _peers_lock(&fpr_net);
    fpr_net.data_callback = callback;
    _peers_unlock(&fpr_net);
}

// ========== STATS / DIAGNOSTICS ==========

void fpr_get_network_stats(fpr_network_stats_t *stats)
{
    fpr_node_get_stats(&fpr_net, stats);
}

void fpr_reset_network_stats(void)
{
    fpr_node_reset_stats(&fpr_net);
}

size_t fpr_cleanup_stale_routes(uint32_t timeout_ms)
{
    return fpr_node_cleanup_stale_routes(&fpr_net, timeout_ms);
}

void fpr_print_route_table(void)
{
    fpr_node_print_route_table(&fpr_net);
}

// ========== HOST MODE ==========

esp_err_t fpr_host_set_config(const fpr_host_config_t *config)
{
    return fpr_node_host_set_config(&fpr_net, config);
}

esp_err_t fpr_host_get_config(fpr_host_config_t *config)
{
    return fpr_node_host_get_config(&fpr_net, config);
}

size_t fpr_host_get_connected_count(void)
{
    return fpr_node_host_connected_count(&fpr_net);
}

esp_err_t fpr_host_approve_peer(const uint8_t *peer_mac)
{
    return fpr_node_host_approve_peer(&fpr_net, peer_mac);
}

esp_err_t fpr_host_reject_peer(const uint8_t *peer_mac)
{
    return fpr_node_host_reject_peer(&fpr_net, peer_mac);
}

esp_err_t fpr_host_block_peer(const uint8_t *peer_mac)
{
    return fpr_node_host_block_peer(&fpr_net, peer_mac);
}

esp_err_t fpr_host_unblock_peer(const uint8_t *peer_mac)
{
    return fpr_node_host_unblock_peer(&fpr_net, peer_mac);
}

esp_err_t fpr_host_disconnect_peer(const uint8_t *peer_mac)
{
    return fpr_node_host_disconnect_peer(&fpr_net, peer_mac);
}

// ========== CLIENT MODE ==========

esp_err_t fpr_client_set_config(const fpr_client_config_t *config)
{
    return fpr_node_client_set_config(&fpr_net, config);
}

esp_err_t fpr_client_get_config(fpr_client_config_t *config)
{
    return fpr_node_client_get_config(&fpr_net, config);
}

size_t fpr_client_list_discovered_hosts(fpr_peer_info_t *peer_array, size_t max_peers)
{
    return fpr_node_client_list_discovered_hosts(&fpr_net, peer_array, max_peers);
}

esp_err_t fpr_client_connect_to_host(const uint8_t *peer_mac, TickType_t timeout)
{
    return fpr_node_client_connect_to_host(&fpr_net, peer_mac, _ticks_to_ms(timeout));
}

esp_err_t fpr_client_disconnect(void)
{
    return fpr_node_client_disconnect(&fpr_net);
}

size_t fpr_client_scan_for_hosts(TickType_t duration)
{
    return fpr_node_client_scan_for_hosts(&fpr_net, _ticks_to_ms(duration));
}

bool fpr_client_is_connected(void)
{
    return fpr_node_client_is_connected(&fpr_net);
}

esp_err_t fpr_client_get_host_info(uint8_t *mac_out, char *name_out, size_t name_size)
{
    return fpr_node_client_get_host_info(&fpr_net, mac_out, name_out, name_size);
}

// ========== LEGACY DYNAMIC TASKS ==========

esp_err_t fpr_network_start_loop_task(TickType_t duration, bool force_restart)
{
    if (fpr_net.state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fpr_net.loop_task != NULL && !force_restart) {
        return ESP_ERR_INVALID_STATE;
    }
    if (force_restart && fpr_net.loop_task != NULL) {
        _fpr_task_stop(&fpr_net, &fpr_net.loop_task, &fpr_net.loop_stop_requested);
    }

    uint32_t duration_ms = (duration == portMAX_DELAY)
                               ? 0 : (uint32_t)duration * portTICK_PERIOD_MS;
    esp_err_t err = fpr_node_start_loop(&fpr_net, duration_ms);
    if (err != ESP_OK) {
        return err;
    }

    const bool host = (fpr_net.current_mode == FPR_MODE_HOST);
    fpr_net.loop_stop_requested = false;
    BaseType_t result = xTaskCreate(_fpr_loop_task_entry,
                                    host ? "FPR_Host_Loop" : "FPR_Client_Loop",
                                    4096, &fpr_net, tskIDLE_PRIORITY + 1,
                                    &fpr_net.loop_task);
    if (result != pdPASS) {
        fpr_net.loop_armed = false;
        return ESP_ERR_NO_MEM;
    }
    taskYIELD();
    return ESP_OK;
}

esp_err_t fpr_network_stop_loop_task(void)
{
    if (fpr_net.loop_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    fpr_net.loop_armed = false;
    _fpr_task_stop(&fpr_net, &fpr_net.loop_task, &fpr_net.loop_stop_requested);
    return ESP_OK;
}

bool fpr_network_is_loop_task_running(void)
{
    return (fpr_net.loop_task != NULL);
}

esp_err_t fpr_network_start_reconnect_task(void)
{
    if (fpr_net.state == FPR_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fpr_net.reconnect_task != NULL) {
        ESP_LOGW(TAG, "Reconnect task already running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = fpr_node_start_reconnect(&fpr_net);
    if (err != ESP_OK) {
        return err;
    }

    const bool host = (fpr_net.current_mode == FPR_MODE_HOST);
    fpr_net.reconnect_stop_requested = false;
    BaseType_t result = xTaskCreatePinnedToCore(
        _fpr_reconnect_task_entry,
        host ? "FPR_Host_Reconnect" : "FPR_Client_Reconnect",
        FPR_TASK_STACK_SIZE, &fpr_net, FPR_TASK_PRIORITY,
        &fpr_net.reconnect_task, FPR_RECONNECT_TASK_CORE_PIN_VALUE);
    if (result == pdPASS) {
        return ESP_OK;
    }
    fpr_net.reconnect_armed = false;
    return ESP_ERR_NO_MEM;
}

esp_err_t fpr_network_stop_reconnect_task(void)
{
    /* Only stops the task — protocol handlers and network state are
     * intentionally untouched, same as the original. */
    if (fpr_net.reconnect_task == NULL) {
        return ESP_OK;
    }
    fpr_net.reconnect_armed = false;
    _fpr_task_stop(&fpr_net, &fpr_net.reconnect_task, &fpr_net.reconnect_stop_requested);
    return ESP_OK;
}

bool fpr_network_is_reconnect_task_running(void)
{
    return (fpr_net.reconnect_task != NULL);
}
