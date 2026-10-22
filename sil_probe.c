/*
 * sil_probe — SIL test-support implementation of fpr_sil.h.
 * Pure packet build/decode helpers; no state, no I/O. The !espnow
 * hook in the sim component calls these to lay wire-format frames into
 * the virtual radio's RX assembly buffer.
 */

#include "fpr/fpr_sil.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fpr/internal/private_defs.h"
#include "fpr/internal/codec.h"
#include "fpr/fpr_lts.h"
#include "fpr/fpr_security.h"

size_t fpr_sil_packet_size(void)
{
    return sizeof(fpr_package_t);
}

size_t fpr_sil_build_control(void *buf, size_t cap,
                             const uint8_t *origin, const uint8_t *dest,
                             const char *name,
                             const uint8_t *pwk, const uint8_t *lwk)
{
    if (buf == NULL || origin == NULL || dest == NULL ||
        cap < sizeof(fpr_package_t)) {
        return 0;
    }
    fpr_package_t pkg_storage;
    fpr_package_t *pkg = &pkg_storage;
    memset(pkg, 0, sizeof(*pkg));
    pkg->id = FPR_PACKET_ID_CONTROL;
    pkg->version = FPR_PROTOCOL_VERSION;
    pkg->package_type = FPR_PACKAGE_TYPE_SINGLE;
    memcpy(pkg->origin_mac, origin, MAC_ADDRESS_LENGTH);
    memcpy(pkg->dest_mac, dest, MAC_ADDRESS_LENGTH);
    pkg->max_hops = FPR_DEFAULT_MAX_HOPS;

    fpr_connect_t *ci = &pkg->protocol.connect_info;
    memset(ci, 0, sizeof(*ci));
    if (name != NULL) {
        strncpy(ci->name, name, sizeof(ci->name) - 1);
    }
    if (pwk != NULL) {
        memcpy(ci->pwk, pwk, FPR_KEY_SIZE);
        ci->has_pwk = true;
    }
    if (lwk != NULL) {
        memcpy(ci->lwk, lwk, FPR_KEY_SIZE);
        ci->has_lwk = true;
    }
    pkg->payload_size = sizeof(*ci);
    return fpr_codec_encode(pkg, buf, cap) != 0 ? sizeof(*pkg) : 0;
}

size_t fpr_sil_build_data(void *buf, size_t cap,
                          const uint8_t *origin, const uint8_t *dest,
                          int id, uint32_t seq, int package_type,
                          const void *payload, size_t payload_len)
{
    if (buf == NULL || origin == NULL || dest == NULL ||
        cap < sizeof(fpr_package_t) ||
        id == FPR_PACKET_ID_CONTROL ||
        payload_len > sizeof(((fpr_package_t *)0)->protocol)) {
        return 0;
    }
    fpr_package_t pkg_storage;
    fpr_package_t *pkg = &pkg_storage;
    memset(pkg, 0, sizeof(*pkg));
    pkg->id = id;
    pkg->version = FPR_PROTOCOL_VERSION;
    pkg->package_type = (fpr_package_type_t)package_type;
    pkg->sequence_num = seq;
    memcpy(pkg->origin_mac, origin, MAC_ADDRESS_LENGTH);
    memcpy(pkg->dest_mac, dest, MAC_ADDRESS_LENGTH);
    pkg->max_hops = FPR_DEFAULT_MAX_HOPS;
    if (payload != NULL && payload_len > 0) {
        memcpy(&pkg->protocol, payload, payload_len);
        pkg->payload_size = (uint16_t)payload_len;
    }
    return fpr_codec_encode(pkg, buf, cap) != 0 ? sizeof(*pkg) : 0;
}

/* Append one field; false when it did not fit — snprintf returns the
 * would-be length on truncation, so advancing p by it would point past
 * the buffer and underflow `left` on the next call. */
static bool _appendf(char **p, size_t *left, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(*p, *left, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= *left) {
        return false;
    }
    *p += (size_t)w;
    *left -= (size_t)w;
    return true;
}

bool fpr_sil_describe(const void *buf, size_t len, char *out, size_t out_len)
{
    if (buf == NULL || out == NULL || len < sizeof(fpr_package_t) ||
        out_len < 96) {
        return false;
    }
    /* Frames longer than the packet still describe fine — decode the
     * wire prefix only, as the old cast-and-read did. */
    fpr_package_t decoded;
    if (!fpr_codec_decode(buf, sizeof(decoded), &decoded)) {
        return false;
    }
    const fpr_package_t *pkg = &decoded;
    int n = snprintf(out, out_len,
             "type=%d id=%d seq=%u orig=%02x:%02x:%02x:%02x:%02x:%02x "
             "dest=%02x:%02x:%02x:%02x:%02x:%02x hop=%u max=%u plen=%u",
             (int)pkg->package_type, (int)pkg->id,
             (unsigned)pkg->sequence_num,
             pkg->origin_mac[0], pkg->origin_mac[1], pkg->origin_mac[2],
             pkg->origin_mac[3], pkg->origin_mac[4], pkg->origin_mac[5],
             pkg->dest_mac[0], pkg->dest_mac[1], pkg->dest_mac[2],
             pkg->dest_mac[3], pkg->dest_mac[4], pkg->dest_mac[5],
             (unsigned)pkg->hop_count, (unsigned)pkg->max_hops,
             (unsigned)pkg->payload_size);
    /* Control packets carry the handshake keys — dump them so a SIL
     * test can echo them back through the handshake state machine.
     * Test path only; keys are random test vectors in SIL. */
    if (pkg->id == FPR_PACKET_ID_CONTROL && n > 0 && (size_t)n < out_len) {
        const fpr_connect_t *ci = &pkg->protocol.connect_info;
        size_t left = out_len - (size_t)n;
        char *p = out + n;
        /* %.*s: the wire name may fill its field with no NUL. A
         * truncated dump still describes the packet — bail early. */
        if (!_appendf(&p, &left, " name=%.*s",
                      (int)sizeof(ci->name), ci->name) ||
            !_appendf(&p, &left, " pwk=")) {
            return true;
        }
        if (ci->has_pwk) {
            for (int i = 0; i < FPR_KEY_SIZE; i++) {
                if (!_appendf(&p, &left, "%02x", ci->pwk[i])) {
                    return true;
                }
            }
        } else if (!_appendf(&p, &left, "-")) {
            return true;
        }
        if (!_appendf(&p, &left, " lwk=")) {
            return true;
        }
        if (ci->has_lwk) {
            for (int i = 0; i < FPR_KEY_SIZE; i++) {
                if (!_appendf(&p, &left, "%02x", ci->lwk[i])) {
                    return true;
                }
            }
        } else {
            _appendf(&p, &left, "-");
        }
    }
    return true;
}
