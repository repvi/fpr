#pragma once

/**
 * @file rx.h
 * @brief FPR internal receive handlers and mode task bodies
 *
 * Entry points registered with esp_now and the task bodies run by
 * fpr_network_start_loop_task()/fpr_network_start_reconnect_task().
 * Not public API — do not call directly.
 */

#include "fpr/internal/helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Client-mode receive handler: host discovery, handshake, data. */
void _handle_client_discovery(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

/** Host-mode receive handler: connection requests, handshake, data. */
void _handle_host_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

/** Extender-mode receive handler: route learning, forwarding, local delivery. */
void _handle_extender_receive(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len);

/** hashmap_foreach callback used by fpr_client_get_host_info(). */
void _find_host_callback(void *key, void *value, void *user_data);

/** Client-mode reconnect/keepalive task body. */
void _fpr_client_reconnect_task(void *arg);

/** Host-mode reconnect/keepalive task body. */
void _fpr_host_reconnect_task(void *arg);

#ifdef __cplusplus
}
#endif
