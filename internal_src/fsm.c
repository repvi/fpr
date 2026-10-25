/**
 * @file fsm.c
 * @brief FPR state machines — node lifecycle, peer connection,
 *        security handshake.
 *
 * Each machine is one transition table: [state][event] -> next state,
 * -1 = illegal edge (ESP_ERR_INVALID_STATE, state unchanged). These
 * tables are the single source of truth — docs/STATE_MACHINE.md
 * renders them and the host FSM test walks them.
 *
 * @version 1.0.0
 * @date October 2026
 */

#include "fpr/internal/fsm.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "fpr_fsm";

#define FSM_ILLEGAL (-1)

/* ============ Node lifecycle ============
 *
 *              START  STOP  PAUSE  RESUME  DEINIT
 * UNINIT       -      -     -      -       -
 * INIT         START  STOP  -      -       UNINIT
 * STARTED      -      STOP  PAUSE  -       -
 * PAUSED       -      STOP  -      STARTED -
 * STOPPED      START  -     -      -       UNINIT
 *
 * STARTED→START and PAUSED→PAUSE stay illegal: a caller that wants
 * restart goes through STOP first; fpr_stop's "already stopped" path
 * is a pre-check in fpr_stop, not a table edge.
 */
static const int8_t s_node_next[FPR_STATE_STOPPED + 1][FPR_NODE_EV_COUNT] = {
    [FPR_STATE_UNINITIALIZED] = { FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL },
    /* DEINIT is legal from any initialized state — the teardown is
     * unconditional (stops tasks, closes transport, frees stores). */
    [FPR_STATE_INITIALIZED]   = { FPR_STATE_STARTED, FPR_STATE_STOPPED, FSM_ILLEGAL, FSM_ILLEGAL, FPR_STATE_UNINITIALIZED },
    [FPR_STATE_STARTED]       = { FSM_ILLEGAL, FPR_STATE_STOPPED, FPR_STATE_PAUSED, FSM_ILLEGAL, FPR_STATE_UNINITIALIZED },
    [FPR_STATE_PAUSED]        = { FSM_ILLEGAL, FPR_STATE_STOPPED, FSM_ILLEGAL, FPR_STATE_STARTED, FPR_STATE_UNINITIALIZED },
    [FPR_STATE_STOPPED]       = { FPR_STATE_STARTED, FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL, FPR_STATE_UNINITIALIZED },
};

esp_err_t fpr_node_transition(fpr_node_t *node, fpr_node_event_t ev)
{
    int8_t next = s_node_next[node->state][ev];
    if (next < 0) {
        ESP_LOGD(TAG, "node ev %d illegal from state %d", (int)ev, (int)node->state);
        return ESP_ERR_INVALID_STATE;
    }
    node->state = (fpr_state_t)next;
    return ESP_OK;
}

/* ============ Peer connection ============
 *
 *                  REQ   REJ   BLOCK  UNBLOCK  ESTAB
 * DISCOVERED       PEND  -     BLOCK  -        CONN
 * PENDING          -     REJ   BLOCK  -        CONN
 * CONNECTED        -     -     BLOCK  -        -
 * REJECTED         PEND  -     BLOCK  -        -
 * BLOCKED          -     -     -      DISC     -
 *
 * ESTABLISH reaches CONNECTED from PENDING (approval flow) and from
 * DISCOVERED (no-PWK auto-approve). Removal deletes the store — a
 * remove is not a state edge. _reset_peer_connection() is the
 * sanctioned forced-reset primitive: it bypasses the table on purpose
 * because it also tears down sec keys, watermark and fragment state.
 */
static const int8_t s_peer_next[FPR_PEER_STATE_BLOCKED + 1][FPR_PEER_EV_COUNT] = {
    [FPR_PEER_STATE_DISCOVERED] = { FPR_PEER_STATE_PENDING, FSM_ILLEGAL,
                                    FPR_PEER_STATE_BLOCKED, FSM_ILLEGAL,
                                    FPR_PEER_STATE_CONNECTED },
    [FPR_PEER_STATE_PENDING]    = { FSM_ILLEGAL, FPR_PEER_STATE_REJECTED,
                                    FPR_PEER_STATE_BLOCKED, FSM_ILLEGAL,
                                    FPR_PEER_STATE_CONNECTED },
    [FPR_PEER_STATE_CONNECTED]  = { FSM_ILLEGAL, FSM_ILLEGAL,
                                    FPR_PEER_STATE_BLOCKED, FSM_ILLEGAL,
                                    FSM_ILLEGAL },
    [FPR_PEER_STATE_REJECTED]   = { FPR_PEER_STATE_PENDING, FSM_ILLEGAL,
                                    FPR_PEER_STATE_BLOCKED, FSM_ILLEGAL,
                                    FSM_ILLEGAL },
    [FPR_PEER_STATE_BLOCKED]    = { FSM_ILLEGAL, FSM_ILLEGAL,
                                    FSM_ILLEGAL, FPR_PEER_STATE_DISCOVERED,
                                    FSM_ILLEGAL },
};

esp_err_t fpr_peer_transition(fpr_peer_t *peer, fpr_peer_event_t ev)
{
    int8_t next = s_peer_next[peer->state][ev];
    if (next < 0) {
        ESP_LOGD(TAG, "peer ev %d illegal from state %d", (int)ev, (int)peer->state);
        return ESP_ERR_INVALID_STATE;
    }
    peer->state = (fpr_peer_state_t)next;
    return ESP_OK;
}

/* ============ Security handshake ============
 *
 *              SEND_PWK   RECV_PWK     SEND_LWK   ESTABLISH   FAIL      REVERT
 * NONE         PWK_SENT   PWK_RCVD     -          ESTABLISHED -         -
 * PWK_SENT     -          -            -          ESTABLISHED  NONE      PWK_SENT
 * PWK_RCVD     -          PWK_RCVD     LWK_SENT   -            PWK_RCVD  -
 * LWK_SENT     -          -            -          ESTABLISHED  PWK_RCVD  -
 * LWK_RCVD     -          -            -          ESTABLISHED  NONE      -
 * ESTABLISHED  -          -            -          -            NONE      PWK_SENT
 *
 * PWK_SENT→ESTABLISH covers the host verifying the client's reply
 * (its LWK arrives in the same packet). The enum order is also the
 * monotonic progression — fpr_sec_at_least() answers "at-or-past".
 */
static const int8_t s_sec_next[FPR_SEC_STATE_ESTABLISHED + 1][FPR_SEC_EV_COUNT] = {
    [FPR_SEC_STATE_NONE]         = { FPR_SEC_STATE_PWK_SENT, FPR_SEC_STATE_PWK_RECEIVED,
                                     FSM_ILLEGAL, FPR_SEC_STATE_ESTABLISHED, FSM_ILLEGAL, FSM_ILLEGAL },
    [FPR_SEC_STATE_PWK_SENT]     = { FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL,
                                     FPR_SEC_STATE_ESTABLISHED, FPR_SEC_STATE_NONE,
                                     FPR_SEC_STATE_PWK_SENT },
    [FPR_SEC_STATE_PWK_RECEIVED] = { FSM_ILLEGAL, FPR_SEC_STATE_PWK_RECEIVED,
                                     FPR_SEC_STATE_LWK_SENT, FSM_ILLEGAL,
                                     FPR_SEC_STATE_PWK_RECEIVED, FSM_ILLEGAL },
    [FPR_SEC_STATE_LWK_SENT]     = { FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL,
                                     FPR_SEC_STATE_ESTABLISHED, FPR_SEC_STATE_PWK_RECEIVED,
                                     FSM_ILLEGAL },
    [FPR_SEC_STATE_LWK_RECEIVED] = { FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL,
                                     FPR_SEC_STATE_ESTABLISHED, FPR_SEC_STATE_NONE,
                                     FSM_ILLEGAL },
    [FPR_SEC_STATE_ESTABLISHED]  = { FSM_ILLEGAL, FSM_ILLEGAL, FSM_ILLEGAL,
                                     FSM_ILLEGAL, FPR_SEC_STATE_NONE, FPR_SEC_STATE_PWK_SENT },
};

esp_err_t fpr_sec_transition(fpr_peer_t *peer, fpr_sec_event_t ev)
{
    int8_t next = s_sec_next[peer->sec_state][ev];
    if (next < 0) {
        ESP_LOGD(TAG, "sec ev %d illegal from state %d", (int)ev, (int)peer->sec_state);
        return ESP_ERR_INVALID_STATE;
    }
    peer->sec_state = (fpr_security_state_t)next;
    return ESP_OK;
}
