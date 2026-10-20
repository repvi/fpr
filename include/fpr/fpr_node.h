#pragma once

/**
 * @file fpr_node.h
 * @brief FPR node instance API — explicit-context alternative to the
 *        process-wide legacy API.
 *
 * A node owns all protocol state (peer map, routes, keys, stats,
 * callbacks, task handles). Multiple nodes may coexist in one process
 * provided each has its own transport (e.g. the host-test fake bus);
 * the real ESP-NOW backend supports exactly one open node because
 * ESP-NOW callbacks carry no context pointer.
 *
 * The legacy fpr_network_* / fpr_client_* / fpr_host_* API in fpr.h is
 * a thin compat layer over a lazily-created default node — existing
 * users are unchanged.
 *
 * Tasks: nodes never own stacks. fpr_node_poll() advances all periodic
 * work in caller-supplied time; fpr_node_start_tasks() optionally runs
 * that polling on caller-provided STATIC task storage
 * (xTaskCreateStaticPinnedToCore). The legacy dynamic-task API still
 * exists in fpr.h.
 *
 * @version 1.0.0
 * @date October 2026
 */

#include "fpr/fpr_def.h"
#include "fpr/fpr_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fpr_node fpr_node_t;

/**
 * @brief Node callbacks. Every callback receives the `user` pointer
 *        from fpr_node_config_t — no process-wide statics needed.
 *        All may be NULL; a NULL selection/request callback means
 *        "auto" exactly like the legacy config fields.
 */
typedef struct {
    void *user;
    void (*on_data)(void *user, const uint8_t *peer_mac, void *data, size_t len);
    bool (*on_connection_request)(void *user, const uint8_t *peer_mac, const char *peer_name, uint32_t peer_key);
    void (*on_peer_discovered)(void *user, const uint8_t *peer_mac, const char *peer_name, int8_t rssi);
    bool (*on_host_selection)(void *user, const uint8_t *peer_mac, const char *peer_name, int8_t rssi);
} fpr_node_callbacks_t;

typedef struct {
    const char *name;                    /* device name (copied) */
    fpr_init_config_t init;              /* channel/power/queue/transport */
    fpr_node_callbacks_t callbacks;      /* ctx-carrying callbacks */
} fpr_node_config_t;

/** Caller-owned static task storage for fpr_node_start_tasks(). */
typedef struct {
    StackType_t *stack;                  /* caller-provided stack buffer */
    uint32_t stack_bytes;                /* sizeof(*stack) in bytes */
    StaticTask_t *tcb;                   /* caller-provided TCB */
    UBaseType_t priority;
    BaseType_t core;                     /* tskNO_AFFINITY for unpinned */
} fpr_task_slot_t;

/**
 * @brief Allocate and initialize a node. One heap allocation; opens no
 *        radio and starts no tasks.
 * @param cfg Node configuration (name + init + callbacks).
 * @param out Receives the node handle.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM.
 */
esp_err_t fpr_node_create(const fpr_node_config_t *cfg, fpr_node_t **out);

/**
 * @brief Stop the node and free it. Legal only after fpr_node_stop() +
 *        fpr_node_stop_tasks(): the transport close has then drained
 *        all RX/TX callbacks and no task references the storage.
 */
void fpr_node_destroy(fpr_node_t *node);

/**
 * @brief Open the transport and mark the node started. Unlike legacy
 *        fpr_network_start(), does NOT reset the mode — set it
 *        explicitly with fpr_node_set_mode() before or after start.
 */
esp_err_t fpr_node_start(fpr_node_t *node);
esp_err_t fpr_node_stop(fpr_node_t *node);
esp_err_t fpr_node_pause(fpr_node_t *node);
esp_err_t fpr_node_resume(fpr_node_t *node);

fpr_network_state_t fpr_node_get_state(fpr_node_t *node);
void fpr_node_set_mode(fpr_node_t *node, fpr_mode_type_t mode);
fpr_mode_type_t fpr_node_get_mode(fpr_node_t *node);
void fpr_node_set_power_mode(fpr_node_t *node, fpr_power_mode_t mode);
fpr_power_mode_t fpr_node_get_power_mode(fpr_node_t *node);
uint8_t fpr_node_get_channel(fpr_node_t *node);
void fpr_node_set_queue_mode(fpr_node_t *node, fpr_queue_mode_t mode);
fpr_queue_mode_t fpr_node_get_queue_mode(fpr_node_t *node);
esp_err_t fpr_node_set_peer_queue_mode(fpr_node_t *node, const uint8_t *peer_mac, fpr_queue_mode_t mode);
uint32_t fpr_node_get_peer_queued_packets(fpr_node_t *node, const uint8_t *peer_mac);
void fpr_node_set_permission_state(fpr_node_t *node, fpr_visibility_t state);
fpr_visibility_t fpr_node_get_permission_state(fpr_node_t *node);

esp_err_t fpr_node_add_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_node_remove_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_node_clear_peers(fpr_node_t *node);
int fpr_node_peer_count(fpr_node_t *node);
size_t fpr_node_get_peers(fpr_node_t *node, fpr_data_receive_cb_t callback, void *user_data);
size_t fpr_node_list_peers(fpr_node_t *node, fpr_peer_info_t *peer_array, size_t max_peers);
esp_err_t fpr_node_get_peer_info(fpr_node_t *node, const uint8_t *peer_mac, fpr_peer_info_t *info);
esp_err_t fpr_node_get_peer_by_name(fpr_node_t *node, const char *peer_name, uint8_t *mac_out);
bool fpr_node_is_peer_reachable(fpr_node_t *node, const uint8_t *peer_mac, uint32_t timeout_ms);

esp_err_t fpr_node_send(fpr_node_t *node, const uint8_t *peer_mac, const void *data, int size, fpr_package_id_t package_id);
esp_err_t fpr_node_send_with_options(fpr_node_t *node, const uint8_t *peer_mac, const void *data, int size, const fpr_send_options_t *options);
esp_err_t fpr_node_broadcast(fpr_node_t *node, const void *data, int size, fpr_package_id_t package_id);
esp_err_t fpr_node_send_device_info(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_node_broadcast_device_info(fpr_node_t *node);

/**
 * @brief Blocking receive (same semantics as the legacy API but the
 *        timeout is plain milliseconds, not FreeRTOS ticks).
 */
bool fpr_node_get_data_from_peer(fpr_node_t *node, const uint8_t *peer_mac, void *data, int data_size, uint32_t timeout_ms);

void fpr_node_get_stats(fpr_node_t *node, fpr_network_stats_t *stats);
void fpr_node_reset_stats(fpr_node_t *node);
size_t fpr_node_cleanup_stale_routes(fpr_node_t *node, uint32_t timeout_ms);
void fpr_node_print_route_table(fpr_node_t *node);

esp_err_t fpr_node_host_set_config(fpr_node_t *node, const fpr_host_config_t *config);
esp_err_t fpr_node_host_get_config(fpr_node_t *node, fpr_host_config_t *config);
size_t fpr_node_host_connected_count(fpr_node_t *node);
esp_err_t fpr_node_host_approve_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_node_host_reject_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_node_host_block_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_node_host_unblock_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_node_host_disconnect_peer(fpr_node_t *node, const uint8_t *peer_mac);

esp_err_t fpr_node_client_set_config(fpr_node_t *node, const fpr_client_config_t *config);
esp_err_t fpr_node_client_get_config(fpr_node_t *node, fpr_client_config_t *config);
size_t fpr_node_client_list_discovered_hosts(fpr_node_t *node, fpr_peer_info_t *peer_array, size_t max_peers);
esp_err_t fpr_node_client_connect_to_host(fpr_node_t *node, const uint8_t *peer_mac, uint32_t timeout_ms);
esp_err_t fpr_node_client_disconnect(fpr_node_t *node);
size_t fpr_node_client_scan_for_hosts(fpr_node_t *node, uint32_t duration_ms);
bool fpr_node_client_is_connected(fpr_node_t *node);
esp_err_t fpr_node_client_get_host_info(fpr_node_t *node, uint8_t *mac_out, char *name_out, size_t name_size);

/**
 * @brief Advance all periodic node work (discovery loop tick +
 *        reconnect/keepalive tick) that is due at @p now_ms.
 *
 * One call performs at most one loop step and one reconnect step —
 * the same bodies the legacy tasks run — then returns the number of
 * milliseconds until the next step is due (0 = call again at once,
 * UINT32_MAX = nothing armed).
 *
 * Work becomes armed by fpr_node_start_loop() /
 * fpr_node_start_reconnect() and disarmed by the matching stop calls
 * or fpr_node_stop().
 */
uint32_t fpr_node_poll(fpr_node_t *node, uint32_t now_ms);

/** Arm/disarm periodic work for fpr_node_poll() (or the node tasks). */
esp_err_t fpr_node_start_loop(fpr_node_t *node, uint32_t duration_ms);
esp_err_t fpr_node_stop_loop(fpr_node_t *node);
esp_err_t fpr_node_start_reconnect(fpr_node_t *node);
esp_err_t fpr_node_stop_reconnect(fpr_node_t *node);

/**
 * @brief Run fpr_node_poll() on caller-owned static tasks. Either slot
 *        may be NULL to skip that task. Cooperative stop: the task
 *        polls its flag once per poll interval, so a task holding the
 *        recursive peers lock is never killed mid-mutation.
 */
esp_err_t fpr_node_start_tasks(fpr_node_t *node, const fpr_task_slot_t *loop_slot, const fpr_task_slot_t *reconnect_slot);
esp_err_t fpr_node_stop_tasks(fpr_node_t *node);
bool fpr_node_tasks_running(fpr_node_t *node);

#ifdef __cplusplus
}
#endif
