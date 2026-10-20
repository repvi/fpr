/**
 * @file codec.c
 * @brief FPR wire codec — struct <-> on-air bytes
 *
 * The over-the-air format is a raw fpr_package_t image: no field-level
 * serialization, no endianness conversion. encode/decode are memcpys by
 * design; they exist to keep every send/recv path on one boundary so
 * the layout stays pinned (test_fpr_codec.c) and swappable.
 */

#include "fpr/internal/codec.h"
#include <string.h>

size_t fpr_codec_encode(const fpr_package_t *pkg, uint8_t *buf, size_t cap)
{
    if (pkg == NULL || buf == NULL || cap < sizeof(fpr_package_t)) {
        return 0;
    }
    memcpy(buf, pkg, sizeof(fpr_package_t));
    return sizeof(fpr_package_t);
}

bool fpr_codec_decode(const uint8_t *buf, size_t len, fpr_package_t *out)
{
    if (buf == NULL || out == NULL || len != sizeof(fpr_package_t)) {
        return false;
    }
    memcpy(out, buf, sizeof(fpr_package_t));
    return true;
}
