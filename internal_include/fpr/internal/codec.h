#pragma once

/**
 * @file codec.h
 * @brief FPR wire codec
 *
 * Single choke point between fpr_package_t and on-air bytes. Today the
 * wire format IS the struct (raw memcpy, no serialization) — these
 * functions exist so every send/receive path funnels through one place
 * and a future versioned format has somewhere to live. The layout is
 * pinned by test_fpr_codec.c.
 *
 * @warning Internal API - subject to change without notice.
 */

#include "fpr/internal/private_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize pkg into buf (cap must be >= sizeof(fpr_package_t)).
 * Returns bytes written, or 0 on failure. */
size_t fpr_codec_encode(const fpr_package_t *pkg, uint8_t *buf, size_t cap);

/* Deserialize a received frame into out. Returns false when len does
 * not match the wire size (caller treats as a malformed packet). */
bool fpr_codec_decode(const uint8_t *buf, size_t len, fpr_package_t *out);

#ifdef __cplusplus
}
#endif
