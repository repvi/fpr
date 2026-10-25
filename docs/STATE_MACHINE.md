# FPR State Machines

Three transition tables own all state movement in FPR —
`internal_src/fsm.c` is the code source of truth, `test_fpr_fsm`
walks every cell. `—` means the edge is illegal: `fpr_*_transition`
returns `ESP_ERR_INVALID_STATE` and leaves the state unchanged.

## Node lifecycle (`fpr_node_transition`)

States: `UNINITIALIZED`, `INITIALIZED`, `STARTED`, `PAUSED`, `STOPPED`.

| State \ Event | START | STOP | PAUSE | RESUME | DEINIT |
|---|---|---|---|---|---|
| UNINITIALIZED | — | — | — | — | — |
| INITIALIZED | STARTED | STOPPED | — | — | UNINITIALIZED |
| STARTED | — | STOPPED | PAUSED | — | UNINITIALIZED |
| PAUSED | — | STOPPED | — | STARTED | UNINITIALIZED |
| STOPPED | STARTED | — | — | — | UNINITIALIZED |

- `fpr_start` only enters STARTED from INITIALIZED or STOPPED (restart
  goes through STOP first). INITIALIZED→STOPPED is legal: `fpr_stop`
  on an initialized-but-never-started node stops it, matching the
  pre-FSM contract. Already-stopped/uninitialized still returns
  `ESP_OK` via a pre-check — not a table edge.
- DEINIT is legal from every initialized state: teardown is
  unconditional (stops tasks, closes transport, frees stores).

## Peer connection (`fpr_peer_transition`)

States: `DISCOVERED`, `PENDING`, `CONNECTED`, `REJECTED`, `BLOCKED`.
Removal paths delete the store outright — they are not table edges.
There is no DISCOVER or APPROVE event: a peer enters the map already
DISCOVERED (creation IS the event), and an approval surfaces as
REQUEST→PENDING then ESTABLISH→CONNECTED.

| State \ Event | REQUEST | REJECT | BLOCK | UNBLOCK | ESTABLISH |
|---|---|---|---|---|---|
| DISCOVERED | PENDING | — | BLOCKED | — | CONNECTED |
| PENDING | — | REJECTED | BLOCKED | — | CONNECTED |
| CONNECTED | — | — | BLOCKED | — | — |
| REJECTED | PENDING | — | BLOCKED | — | — |
| BLOCKED | — | — | — | DISCOVERED | — |

- `ESTABLISH` reaches CONNECTED from both PENDING (normal approval
  flow) and DISCOVERED (no-PWK / auto-approve immediate connect).
- `BLOCK` is legal from every non-blocked state — blocking always wins.
- `REJECT` fires from PENDING (`fpr_host_reject`); rejecting a
  non-PENDING peer goes through `_reset_peer_connection` directly —
  the reset primitive owns the field teardown (sec keys, watermark,
  fragment state), so it bypasses the table deliberately.
- There is no DISCONNECT event: teardown goes through
  `_reset_peer_connection`, which overwrites `state` unconditionally —
  a table edge there would be dead work. Peer removal likewise
  deletes the store outright.
- `peer->is_connected` no longer exists: connected means
  `state == FPR_PEER_STATE_CONNECTED` everywhere; the public
  `fpr_peer_info_t.is_connected` is derived at copy time.

## Security handshake (`fpr_sec_transition`)

States: `NONE`, `PWK_SENT`, `PWK_RECEIVED`, `LWK_SENT`,
`LWK_RECEIVED`, `ESTABLISHED`.

| State \ Event | SEND_PWK | RECV_PWK | SEND_LWK | ESTABLISH | FAIL | REVERT |
|---|---|---|---|---|---|---|
| NONE | PWK_SENT | PWK_RECEIVED | — | ESTABLISHED | — | — |
| PWK_SENT | — | — | — | ESTABLISHED | NONE | PWK_SENT |
| PWK_RECEIVED | — | PWK_RECEIVED | LWK_SENT | — | PWK_RECEIVED | — |
| LWK_SENT | — | — | — | ESTABLISHED | PWK_RECEIVED | — |
| LWK_RECEIVED | — | — | — | ESTABLISHED | NONE | — |
| ESTABLISHED | — | — | — | — | NONE | PWK_SENT |

- `ESTABLISH` from NONE covers the no-security direct-connect path.
- `FAIL` reverts to the pre-send stage (send error); `REVERT` re-arms
  the PWK wait after a failed client reply — including the
  verify-and-ack send-failure path that had already reached
  ESTABLISHED (ESTABLISHED→PWK_SENT), which is what un-wedges a host
  whose ack frame was lost.
- RECV_PWK self-loops at PWK_RECEIVED: a duplicate PWK re-heard while
  waiting to send the LWK is benign and must not regress the handshake.
- The enum order is the monotonic progression —
  `fpr_sec_at_least(peer, min)` answers "at-or-past stage" queries.

## Mode × capability matrix (`s_mode_ops` in fpr.c)

| Mode | Broadcast peer | RX handler | Host PWK |
|---|---|---|---|
| DEFAULT | — | (unchanged) | — |
| CLIENT | yes ("client") | `_rx_client` | — |
| HOST | yes ("host") | `_rx_host` | yes |
| BROADCAST | — | (unchanged) | — |
| EXTENDER | yes ("extender") | `_rx_extender` | — |

Derived state — no shadow flags: `paused` was removed (use
`state == FPR_STATE_PAUSED`), `routing_enabled` was removed
(`current_mode == FPR_MODE_EXTENDER`), `is_connected` removed
(`state == FPR_PEER_STATE_CONNECTED`).
