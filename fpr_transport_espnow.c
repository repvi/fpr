/**
 * @file fpr_transport_espnow.c
 * @brief ESP-NOW backend for the FPR transport port
 *
 * This is the only fpr file allowed to include esp_now.h/esp_wifi.h —
 * tools enforce that (test_fpr_build.py). Everything else reaches the
 * radio through fpr_transport_t.
 *
 * Global-state note: ESP-NOW's send/recv callbacks carry no user
 * context, so the adapter keeps one static sink — an inherent
 * single-radio limitation (one ESP-NOW instance per device), not a
 * design choice. This is the successor to the old FprService static
 * pointer exception.
 *
 * @version 1.0.0
 * @date October 2026
 */

#include "fpr/fpr_transport.h"
#include "fpr/internal/private_defs.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "fpr_espnow";

_Static_assert(ESP_NOW_MAX_DATA_LEN > sizeof(fpr_package_t),
               "ESP_NOW_MAX_DATA_LEN must be greater than sizeof(fpr_package_t)");

static struct {
    fpr_transport_rx_fn rx;
    fpr_transport_tx_done_fn tx_done;
    void *sink;
} s_radio;

static void _espnow_rx(const esp_now_recv_info_t *info,
                       const uint8_t *data, int len)
{
    if (s_radio.rx == NULL) {
        return;
    }
    const fpr_rx_frame_t frame = {
        .src_mac = info ? info->src_addr : NULL,
        .dst_mac = info ? info->des_addr : NULL,
        /* rx_ctrl is driver-provided but not documented as always
         * non-NULL — report 0 ("unknown") when absent. */
        .rssi = (info && info->rx_ctrl) ? info->rx_ctrl->rssi : 0,
        .data = data,
        .len = (size_t)len,
    };
    s_radio.rx(s_radio.sink, &frame);
}

static void _espnow_tx_done(const wifi_tx_info_t *tx_info,
                            esp_now_send_status_t status)
{
    if (s_radio.tx_done != NULL) {
        s_radio.tx_done(s_radio.sink,
                        tx_info ? tx_info->des_addr : NULL,
                        status == ESP_NOW_SEND_SUCCESS);
    }
}

static esp_err_t _open(void *ctx, fpr_transport_rx_fn rx,
                       fpr_transport_tx_done_fn tx_done, void *sink)
{
    (void)ctx;
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_radio.rx = rx;
    s_radio.tx_done = tx_done;
    s_radio.sink = sink;
    err = esp_now_register_recv_cb(_espnow_rx);
    if (err == ESP_OK) {
        err = esp_now_register_send_cb(_espnow_tx_done);
    }
    if (err != ESP_OK) {
        s_radio.rx = NULL;
        s_radio.tx_done = NULL;
        s_radio.sink = NULL;
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        return err;
    }
    return ESP_OK;
}

static esp_err_t _close(void *ctx)
{
    (void)ctx;
    /* Unregister before deinit: nothing new is dispatched on the wifi
     * task after this point — only callbacks already in flight remain,
     * and the caller drains those after close() returns. */
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_err_t err = esp_now_deinit();
    s_radio.rx = NULL;
    s_radio.tx_done = NULL;
    s_radio.sink = NULL;
    return err;
}

static esp_err_t _add_peer(void *ctx, const uint8_t mac[6],
                           uint8_t channel, bool encrypt)
{
    (void)ctx;
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, MAC_ADDRESS_LENGTH);
    peer.channel = channel;      /* 0 = current channel */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = encrypt;
    /* ESP-NOW rejects duplicate adds; del-first is the existing fpr
     * semantics (refresh a peer's table entry). */
    esp_now_del_peer(mac);
    return esp_now_add_peer(&peer);
}

static esp_err_t _del_peer(void *ctx, const uint8_t mac[6])
{
    (void)ctx;
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_now_del_peer(mac);
}

static esp_err_t _send(void *ctx, const uint8_t mac[6],
                       const uint8_t *data, size_t len)
{
    (void)ctx;
    if (data == NULL || len > ESP_NOW_MAX_DATA_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    /* mac may be NULL to mean broadcast — ESP-NOW accepts that form.
     * Map the backend's queue-full error onto the generic one so the
     * core never has to name an ESP-NOW constant. */
    esp_err_t err = esp_now_send(mac, data, len);
    return (err == ESP_ERR_ESPNOW_NO_MEM) ? ESP_ERR_NO_MEM : err;
}

static esp_err_t _set_channel(void *ctx, uint8_t channel)
{
    (void)ctx;
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

static esp_err_t _get_mac(void *ctx, uint8_t mac[6])
{
    (void)ctx;
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

static bool _ready(void *ctx)
{
    (void)ctx;
    wifi_mode_t mode;
    return esp_wifi_get_mode(&mode) == ESP_OK &&
           (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
}

const fpr_transport_t *fpr_transport_espnow(void)
{
    static const fpr_transport_t transport = {
        .ctx = NULL,
        .mtu = ESP_NOW_MAX_DATA_LEN,
        .open = _open,
        .close = _close,
        .add_peer = _add_peer,
        .del_peer = _del_peer,
        .send = _send,
        .set_channel = _set_channel,
        .get_mac = _get_mac,
        .ready = _ready,
    };
    return &transport;
}
