/**
 * @file rx.c
 * @brief Shared receive-path preamble for the FPR mode handlers
 *
 * The client, host, and extender RX handlers all start with the same
 * gates — paused, packet size, protocol-version dispatch — and then
 * enter peers_lock to work on the peer map. _rx_begin(node) owns that
 * preamble so the quiesce/lock discipline lives in one place.
 */

#include "fpr/internal/rx.h"
#include "fpr/internal/fpr_handle.h"
#include "fpr/internal/codec.h"
#include <string.h>

/* Stat increments go through peers_lock like every other stats write;
 * the lock is recursive so this is safe whether or not a caller already
 * holds it. */
static void _rx_drop(fpr_node_t *node, bool count_drops)
{
    if (!count_drops) {
        return;
    }
    _peers_lock(node);
    node->stats.packets_dropped++;
    _peers_unlock(node);
}

bool _rx_begin(fpr_node_t *node, const fpr_rx_frame_t *frame, const uint8_t *data, int len,
               bool count_drops, fpr_rx_ctx_t *ctx)
{
    if (node->paused) {
        _rx_drop(node, count_drops);
        return false;
    }

    if (!is_fpr_package_compatible(len)) {
        _rx_drop(node, count_drops);
        return false;
    }

    /* Extender accounting counts a received packet once it is size-valid,
     * before version dispatch may reject it. */
    if (count_drops) {
        _peers_lock(node);
        node->stats.packets_received++;
        _peers_unlock(node);
    }

    /* Decode into handler-owned storage — the driver buffer stays
     * untouched and every path below reads the same snapshot. */
    if (!fpr_codec_decode(data, (size_t)len, &ctx->decoded)) {
        _rx_drop(node, count_drops);
        return false;
    }
    const fpr_package_t *package = &ctx->decoded;

    if (!fpr_version_dispatch(frame, data, len, package->version)) {
        _rx_drop(node, count_drops);
        return false;
    }

    /* Sanitize a local control message, never the driver-owned packet:
     * the same union holds application bytes for non-control packets. */
    ctx->is_control = (package->id == FPR_PACKET_ID_CONTROL);
    memset(&ctx->control, 0, sizeof(ctx->control));
    if (ctx->is_control) {
        memcpy(&ctx->control, &package->protocol.connect_info, sizeof(ctx->control));
        ctx->control.name[FPR_CONNECT_NAME_SIZE - 1] = '\0';
    }
    ctx->package = package;
    ctx->is_broadcast = is_broadcast_address(frame->dst_mac);

    /* Serialize the peer-map region against remove/clear on other tasks.
     * Recursive: mode handlers and their callbacks re-enter peer APIs on
     * this same task. The quiesce gate is re-checked inside the lock —
     * deinit flips state to UNINITIALIZED before draining, so a callback
     * dispatched just before unregister may only reach the lock now. */
    _peers_lock(node);
    if (node->state == FPR_STATE_UNINITIALIZED) {
        _peers_unlock(node);
        return false;
    }
    return true;
}
