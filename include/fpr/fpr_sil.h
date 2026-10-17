#pragma once
/*
 * fpr_sil — SIL test-support API (CONFIG_RAMINOX_SIL builds only).
 *
 * The sim component's virtual ESP-NOW radio injects wire-format
 * fpr_package_t frames, but that type lives in fpr's private headers.
 * These pure helpers let the !espnow hook build and decode packets
 * without exporting the layout. Compiled only in SIL builds
 * (sil_probe.c); callers must be under the same guard.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sizeof(fpr_package_t) — sanity anchor for host-side packing. */
size_t fpr_sil_packet_size(void);

/* Build a control (discovery/handshake) packet into buf.
 * pwk/lwk NULL means the key flag stays clear. Returns the packet
 * length, or 0 on bad args/capacity. */
size_t fpr_sil_build_control(void *buf, size_t cap,
                             const uint8_t *origin, const uint8_t *dest,
                             const char *name,
                             const uint8_t *pwk, const uint8_t *lwk);

/* Build a data packet: package_type 0-3 (SINGLE/START/CONTINUED/END),
 * arbitrary app id (anything but the reserved control id), payload up
 * to the protocol field size. Returns packet length or 0. */
size_t fpr_sil_build_data(void *buf, size_t cap,
                          const uint8_t *origin, const uint8_t *dest,
                          int id, uint32_t seq, int package_type,
                          const void *payload, size_t payload_len);

/* Decode a wire packet into a single "k=v k=v ..." line:
 *   type=<n> id=<n> seq=<u> orig=<mac> dest=<mac> hop=<n> max=<n> plen=<n>
 * Returns false when buf isn't a plausible fpr_package_t. */
bool fpr_sil_describe(const void *buf, size_t len, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
