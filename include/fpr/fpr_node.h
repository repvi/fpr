#pragma once

/**
 * @file fpr_node.h
 * @brief FPR node API — every operation takes an explicit node handle.
 *
 * A node owns all protocol state (peer map, routes, keys, stats,
 * callbacks, task handles). Multiple nodes may coexist in one process
 * provided each has its own transport (e.g. the host-test fake bus);
 * the real ESP-NOW backend supports exactly one open node because
 * ESP-NOW callbacks carry no context pointer.
 *
 * Tasks: nodes never own stacks. fpr_poll() advances all periodic work
 * in caller-supplied time; fpr_start_loop_task() /
 * fpr_start_reconnect_task() optionally run that polling on
 * caller-provided STATIC task storage (xTaskCreateStaticPinnedToCore).
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

/** Application-data callback — receives the config-supplied user ptr. */
typedef void (*fpr_data_cb_t)(void *user, const uint8_t *peer_mac, void *data, size_t len);

/** Peer-iteration callback for fpr_foreach_peer() — @p peer is the
 *  internal peer store (opaque); use fpr_peer_info() for details. */
typedef void (*fpr_peer_cb_t)(void *user, const uint8_t *peer_mac, void *peer);

/**
 * @brief Node callbacks. Every callback receives the `user` pointer —
 *        no process-wide statics needed. All may be NULL; a NULL
 *        selection/request callback means "auto".
 */
typedef struct {
    void *user;
    fpr_data_cb_t on_data;
    bool (*on_connection_request)(void *user, const uint8_t *peer_mac, const char *peer_name, uint32_t peer_key);
    void (*on_peer_discovered)(void *user, const uint8_t *peer_mac, const char *peer_name, int8_t rssi);
    bool (*on_host_selection)(void *user, const uint8_t *peer_mac, const char *peer_name, int8_t rssi);
} fpr_node_callbacks_t;

typedef struct {
    const char *name;                    /* device name (copied) */
    fpr_init_config_t init;              /* channel/power/queue/transport */
    fpr_node_callbacks_t callbacks;      /* ctx-carrying callbacks */
} fpr_node_config_t;

/** Caller-owned static task storage for the task-start APIs. */
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
esp_err_t fpr_create(const fpr_node_config_t *cfg, fpr_node_t **out);

/**
 * @brief Initialize a node into caller-provided storage (static or
 *        embedded). The caller must have the full definition — include
 *        the private header. Pair with fpr_deinit(), not fpr_destroy().
 */
esp_err_t fpr_init_at(fpr_node_t *node, const char *name,
                      const fpr_init_config_t *config);
esp_err_t fpr_deinit(fpr_node_t *node);

/**
 * @brief Stop the node and free it. Legal only after fpr_stop() +
 *        fpr_stop_loop_task()/fpr_stop_reconnect_task(): the transport
 *        close has then drained all RX/TX callbacks and no task
 *        references the storage.
 */
void fpr_destroy(fpr_node_t *node);

/** Replace the callback table (all ctx-carrying). NULL clears. */
esp_err_t fpr_set_callbacks(fpr_node_t *node, const fpr_node_callbacks_t *callbacks);

/**
 * @brief Open the transport and mark the node started. A node that
 *        never picked a mode starts as CLIENT; an explicit
 *        fpr_set_mode() before start is honored.
 */
esp_err_t fpr_start(fpr_node_t *node);
esp_err_t fpr_stop(fpr_node_t *node);
esp_err_t fpr_pause(fpr_node_t *node);
esp_err_t fpr_resume(fpr_node_t *node);

fpr_state_t fpr_get_state(fpr_node_t *node);
void fpr_set_mode(fpr_node_t *node, fpr_mode_type_t mode);
fpr_mode_type_t fpr_get_mode(fpr_node_t *node);
void fpr_set_power_mode(fpr_node_t *node, fpr_power_mode_t mode);
fpr_power_mode_t fpr_get_power_mode(fpr_node_t *node);
uint8_t fpr_get_channel(fpr_node_t *node);
void fpr_set_queue_mode(fpr_node_t *node, fpr_queue_mode_t mode);
fpr_queue_mode_t fpr_get_queue_mode(fpr_node_t *node);
esp_err_t fpr_set_peer_queue_mode(fpr_node_t *node, const uint8_t *peer_mac, fpr_queue_mode_t mode);
uint32_t fpr_peer_queued_packets(fpr_node_t *node, const uint8_t *peer_mac);
void fpr_set_visibility(fpr_node_t *node, fpr_visibility_t state);
fpr_visibility_t fpr_get_visibility(fpr_node_t *node);

esp_err_t fpr_add_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_remove_peer(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_clear_peers(fpr_node_t *node);
int fpr_peer_count(fpr_node_t *node);
size_t fpr_foreach_peer(fpr_node_t *node, fpr_peer_cb_t callback, void *user);
size_t fpr_list_peers(fpr_node_t *node, fpr_peer_info_t *peer_array, size_t max_peers);
esp_err_t fpr_peer_info(fpr_node_t *node, const uint8_t *peer_mac, fpr_peer_info_t *info);
esp_err_t fpr_peer_by_name(fpr_node_t *node, const char *peer_name, uint8_t *mac_out);
bool fpr_peer_reachable(fpr_node_t *node, const uint8_t *peer_mac, uint32_t timeout_ms);

esp_err_t fpr_send(fpr_node_t *node, const uint8_t *peer_mac, const void *data, int size, fpr_package_id_t package_id);
esp_err_t fpr_send_opts(fpr_node_t *node, const uint8_t *peer_mac, const void *data, int size, const fpr_send_options_t *options);
esp_err_t fpr_broadcast(fpr_node_t *node, const void *data, int size, fpr_package_id_t package_id);
esp_err_t fpr_send_device_info(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_broadcast_device_info(fpr_node_t *node);

/**
 * @brief Blocking receive from one peer; @p timeout_ms is plain
 *        milliseconds (UINT32_MAX = wait forever).
 */
bool fpr_recv_from(fpr_node_t *node, const uint8_t *peer_mac, void *data, int data_size, uint32_t timeout_ms);

void fpr_get_stats(fpr_node_t *node, fpr_stats_t *stats);
void fpr_reset_stats(fpr_node_t *node);
size_t fpr_prune_routes(fpr_node_t *node, uint32_t timeout_ms);
void fpr_print_routes(fpr_node_t *node);

esp_err_t fpr_host_set_config(fpr_node_t *node, const fpr_host_config_t *config);
esp_err_t fpr_host_get_config(fpr_node_t *node, fpr_host_config_t *config);
size_t fpr_host_peer_count(fpr_node_t *node);
esp_err_t fpr_host_approve(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_host_reject(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_host_block(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_host_unblock(fpr_node_t *node, const uint8_t *peer_mac);
esp_err_t fpr_host_disconnect(fpr_node_t *node, const uint8_t *peer_mac);

esp_err_t fpr_client_set_config(fpr_node_t *node, const fpr_client_config_t *config);
esp_err_t fpr_client_get_config(fpr_node_t *node, fpr_client_config_t *config);
size_t fpr_client_hosts(fpr_node_t *node, fpr_peer_info_t *peer_array, size_t max_peers);
esp_err_t fpr_client_connect(fpr_node_t *node, const uint8_t *peer_mac, uint32_t timeout_ms);
esp_err_t fpr_client_disconnect(fpr_node_t *node);
size_t fpr_client_scan(fpr_node_t *node, uint32_t duration_ms);
bool fpr_client_connected(fpr_node_t *node);
esp_err_t fpr_client_host_info(fpr_node_t *node, uint8_t *mac_out, char *name_out, size_t name_size);

/**
 * @brief Advance all periodic node work (discovery loop tick +
 *        reconnect/keepalive tick) that is due at @p now_ms.
 *
 * One call performs at most one loop step and one reconnect step —
 * the same bodies the node tasks run — then returns the number of
 * milliseconds until the next step is due (0 = call again at once,
 * UINT32_MAX = nothing armed).
 *
 * Work becomes armed by fpr_start_loop() / fpr_start_reconnect() (or
 * the *_task variants, which arm too) and disarmed by the matching
 * stop calls or fpr_stop().
 */
uint32_t fpr_poll(fpr_node_t *node, uint32_t now_ms);

/** Arm/disarm periodic work for fpr_poll() (or the node tasks). */
esp_err_t fpr_start_loop(fpr_node_t *node, uint32_t duration_ms);
esp_err_t fpr_stop_loop(fpr_node_t *node);
esp_err_t fpr_start_reconnect(fpr_node_t *node);
esp_err_t fpr_stop_reconnect(fpr_node_t *node);

/**
 * @brief Run the periodic work on caller-owned static tasks. The task
 *        polls its stop flag once per poll interval, so a task holding
 *        the recursive peers lock is never killed mid-mutation.
 *
 * fpr_start_loop_task() arms the loop AND spawns the task; a running
 * loop task is rejected unless @p force_restart (which stops the old
 * task first). fpr_stop_loop_task() is ESP_ERR_INVALID_STATE when no
 * loop task runs. The reconnect pair keeps the historical asymmetry:
 * stop is a no-op when nothing runs.
 */
esp_err_t fpr_start_loop_task(fpr_node_t *node, uint32_t duration_ms,
                              const fpr_task_slot_t *slot, bool force_restart);
esp_err_t fpr_stop_loop_task(fpr_node_t *node);
bool fpr_loop_task_running(fpr_node_t *node);
esp_err_t fpr_start_reconnect_task(fpr_node_t *node, const fpr_task_slot_t *slot);
esp_err_t fpr_stop_reconnect_task(fpr_node_t *node);
bool fpr_reconnect_task_running(fpr_node_t *node);

#ifdef __cplusplus
}
#endif
