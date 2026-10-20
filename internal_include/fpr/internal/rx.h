#pragma once

/**
 * @file rx.h
 * @brief FPR internal receive handlers and mode task bodies
 *
 * Entry points dispatched from the transport RX path and the task
 * bodies run by fpr_network_start_loop_task()/_start_reconnect_task().
 * Not public API — do not call directly.
 */

#include "fpr/internal/helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of the shared RX preamble: the parsed packet plus the sanitized
 * control payload. `control` is zeroed for non-control packets; its name
 * field is always NUL-terminated. */
typedef struct {
    fpr_package_t decoded;         /* codec output — this stack copy is
                                    * the only packet the handler reads */
    const fpr_package_t *package;  /* == &decoded */
    fpr_connect_t control;
    bool is_control;   /* package->id == FPR_PACKET_ID_CONTROL */
    bool is_broadcast; /* frame destination is the broadcast MAC */
} fpr_rx_ctx_t;

/* Shared RX preamble used by every mode handler: paused/size/version
 * gates, sanitized control copy, broadcast detect, then peers_lock +
 * quiesce re-check (rejects while state is UNINITIALIZED).
 *
 * On true return peers_lock is HELD and ctx is populated — the caller
 * must _peers_unlock() before returning. On false the packet was
 * rejected and no lock is held. count_drops charges rejects to
 * stats.packets_dropped and counts size-valid packets in
 * stats.packets_received (extender accounting; other modes pass false). */
bool _rx_begin(fpr_node_t *net, const fpr_rx_frame_t *frame,
               const uint8_t *data, int len,
               bool count_drops, fpr_rx_ctx_t *ctx);

/** Client-mode receive handler: host discovery, handshake, data. */
void _rx_client(fpr_node_t *net, const fpr_rx_frame_t *frame, const uint8_t *data, int len);

/** Host-mode receive handler: connection requests, handshake, data. */
void _rx_host(fpr_node_t *net, const fpr_rx_frame_t *frame, const uint8_t *data, int len);

/** Extender-mode receive handler: route learning, forwarding, local delivery. */
void _rx_extender(fpr_node_t *net, const fpr_rx_frame_t *frame, const uint8_t *data, int len);

/* Cooperative task exit shared by the task bodies in fpr.c, fpr_client.c
 * and fpr_host.c — implemented in fpr.c next to _task_stop. */
void _task_exit(fpr_node_t *net, TaskHandle_t *slot);

/* Cooperative-stop helper + the unified task entries — used by the
 * node static-task API and the compat layer's dynamic tasks. */
void _fpr_task_stop(fpr_node_t *net, TaskHandle_t *handle, volatile bool *stop_flag);
void _fpr_loop_task_entry(void *arg);       /* arg = fpr_node_t* */
void _fpr_reconnect_task_entry(void *arg);  /* arg = fpr_node_t* */

/* Single-step bodies shared by the legacy tasks, the node tasks and
 * fpr_node_poll() — each performs one iteration of the periodic work. */
void _client_loop_step(fpr_node_t *net);
void _host_loop_step(fpr_node_t *net);
void _client_reconnect_step(fpr_node_t *net, uint32_t now_ms);
void _host_reconnect_step(fpr_node_t *net, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
