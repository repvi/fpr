#pragma once

/**
 * @file fpr_transport.h
 * @brief FPR radio transport port
 *
 * FPR core talks to the radio only through this interface. The default
 * backend is ESP-NOW (`fpr_transport_espnow()`); SIL/tests inject an
 * in-memory transport instead — no linker interposition needed.
 *
 * Threading contract: `rx`/`tx_done` are invoked from the transport's
 * RX context (the WiFi task under ESP-NOW). `close()` must guarantee no
 * further `rx()`/`tx_done()` calls after it returns. Peer-table and
 * `send()` calls may come from any task once `open()` has returned.
 *
 * @version 1.0.0
 * @date October 2026
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One received frame, transport-agnostic view of esp_now_recv_info_t. */
typedef struct {
    const uint8_t *src_mac;  /* sender MAC (6 bytes) */
    const uint8_t *dst_mac;  /* packet destination MAC (6 bytes) */
    int8_t rssi;             /* 0 when the transport cannot report RSSI */
    const uint8_t *data;     /* wire bytes, owned by the transport */
    size_t len;
} fpr_rx_frame_t;

typedef void (*fpr_transport_rx_fn)(void *sink, const fpr_rx_frame_t *frame);
typedef void (*fpr_transport_tx_done_fn)(void *sink, const uint8_t *mac, bool ok);

typedef struct fpr_transport {
    void *ctx;
    size_t mtu;             /* max payload bytes per frame (ESP-NOW: 250) */
    /** Bring the radio up and install the RX/TX sinks. */
    esp_err_t (*open)(void *ctx, fpr_transport_rx_fn rx,
                      fpr_transport_tx_done_fn tx_done, void *sink);
    /** Quiesce callbacks, then tear the radio down. */
    esp_err_t (*close)(void *ctx);
    esp_err_t (*add_peer)(void *ctx, const uint8_t mac[6],
                          uint8_t channel, bool encrypt);
    esp_err_t (*del_peer)(void *ctx, const uint8_t mac[6]);
    esp_err_t (*send)(void *ctx, const uint8_t mac[6],
                      const uint8_t *data, size_t len);
    esp_err_t (*set_channel)(void *ctx, uint8_t channel);
    /** Local MAC of this station (6 bytes). */
    esp_err_t (*get_mac)(void *ctx, uint8_t mac[6]);
    /** Radio is up and in a mode FPR can send on (STA/APSTA). */
    bool (*ready)(void *ctx);
} fpr_transport_t;

/** Default ESP-NOW backend. Returns a shared static transport — ESP-NOW
 * is a single-radio API with context-less callbacks, so at most one
 * node may use it at a time (see fpr_transport_espnow.c). */
const fpr_transport_t *fpr_transport_espnow(void);

#ifdef __cplusplus
}
#endif
