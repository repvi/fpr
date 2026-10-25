#pragma once

/**
 * @file fsm.h
 * @brief FPR internal state machines — node lifecycle, peer
 *        connection, and the security handshake.
 *
 * Every state transition goes through these tables instead of open
 * `->state = X` writes. A transition the table marks illegal returns
 * ESP_ERR_INVALID_STATE; callers keep their transport/send side
 * effects, the table only owns "is this state edge allowed".
 *
 * The tables are the source of truth for docs/STATE_MACHINE.md —
 * the rendered charts there are generated from these rows.
 *
 * @version 1.0.0
 * @date October 2026
 */

#include "fpr/internal/private_defs.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Node lifecycle ---- */

typedef enum {
    FPR_NODE_EV_START = 0,   /* fpr_start */
    FPR_NODE_EV_STOP,        /* fpr_stop  */
    FPR_NODE_EV_PAUSE,       /* fpr_pause */
    FPR_NODE_EV_RESUME,      /* fpr_resume */
    FPR_NODE_EV_DEINIT,      /* fpr_deinit */
    FPR_NODE_EV_COUNT
} fpr_node_event_t;

/* Applies the lifecycle edge; returns the resulting state or
 * ESP_ERR_INVALID_STATE (state left unchanged). */
esp_err_t fpr_node_transition(fpr_node_t *node, fpr_node_event_t ev);

/* ---- Peer connection ---- */

typedef enum {
    /* DISCOVER, APPROVE, DISCONNECT are not transitions: a peer enters
     * the table already DISCOVERED (creation IS the event), approval
     * surfaces as REQUEST->PENDING then ESTABLISH->CONNECTED, and
     * disconnect teardown goes through _reset_peer_connection — which
     * overwrites state unconditionally, so a table edge would be dead
     * work. */
    FPR_PEER_EV_REQUEST = 0,  /* peer requested a connection        */
    FPR_PEER_EV_REJECT,
    FPR_PEER_EV_BLOCK,
    FPR_PEER_EV_UNBLOCK,
    FPR_PEER_EV_ESTABLISH,    /* security handshake completed        */
    FPR_PEER_EV_COUNT
} fpr_peer_event_t;

esp_err_t fpr_peer_transition(fpr_peer_t *peer, fpr_peer_event_t ev);

/* ---- Security handshake ---- */

typedef enum {
    FPR_SEC_EV_SEND_PWK = 0,  /* host sent its PWK                   */
    FPR_SEC_EV_RECV_PWK,      /* client received + stored the PWK    */
    FPR_SEC_EV_SEND_LWK,      /* client/host sent its LWK            */
    FPR_SEC_EV_ESTABLISH,     /* last ACK verified — session up      */
    FPR_SEC_EV_FAIL,          /* send failed — revert to pre-send    */
    FPR_SEC_EV_REVERT,        /* verify failed — re-arm PWK wait     */
    FPR_SEC_EV_COUNT
} fpr_sec_event_t;

esp_err_t fpr_sec_transition(fpr_peer_t *peer, fpr_sec_event_t ev);

/* "At-or-past" predicate — several handshake guards mean "have we
 * reached at least this stage", not "exactly". */
static inline bool fpr_sec_at_least(const fpr_peer_t *peer,
                                    fpr_security_state_t min)
{
    return peer != NULL && peer->sec_state >= min;
}

#ifdef __cplusplus
}
#endif
