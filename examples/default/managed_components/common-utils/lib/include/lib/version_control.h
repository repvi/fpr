#ifndef VERSION_CONTROL_H
#define VERSION_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Protocol version type (encoded as major.minor.patch in uint32).
 * Format: [major:8][minor:8][patch:8][reserved:8]
 */
typedef uint32_t code_version_t;

// Version extraction macros (extract from packed version)
#define CODE_VERSION_MAJOR(version) (((version) >> 16) & 0xFF)
#define CODE_VERSION_MINOR(version) (((version) >> 8) & 0xFF)
#define CODE_VERSION_PATCH(version) ((version) & 0xFF)

// Version creation macro (pack major.minor.patch into uint32)
#define CODE_VERSION(major, minor, patch) \
    ((((code_version_t)(major) & 0xFF) << 16) | \
     (((code_version_t)(minor) & 0xFF) << 8) | \
     ((code_version_t)(patch) & 0xFF))

// Version comparison macros
#define CODE_VERSION_AT_LEAST(version, min_version) ((version) >= (min_version))
#define CODE_VERSION_LESS_THAN(version, other) ((version) < (other))
#define CODE_VERSION_EQUAL(v1, v2) ((v1) == (v2))

// Major version comparison (for breaking changes)
#define CODE_VERSION_SAME_MAJOR(v1, v2) (CODE_VERSION_MAJOR(v1) == CODE_VERSION_MAJOR(v2))

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if incoming packet version is compatible with minimum supported
 * @param packet_version Version from received packet
 * @param min_supported_version Minimum version we can handle
 * @return true if compatible, false if should be rejected
 */
static inline bool is_version_compatible(code_version_t packet_version, code_version_t min_supported_version)
{
    // Version 0 means unversioned legacy packet (pre-versioning era)
    if (packet_version == 0) {
        return true; // Allow, but handle specially
    }
    // Reject versions below minimum supported
    return CODE_VERSION_AT_LEAST(packet_version, min_supported_version);
}

/**
 * @brief Check if incoming packet requires legacy handling
 * @param packet_version Version from received packet
 * @param our_version Our current protocol version
 * @return true if legacy handler should be used
 */
static inline bool requires_legacy_handler(code_version_t packet_version, code_version_t our_version)
{
    // Version 0 or major version mismatch requires legacy handling
    if (packet_version == 0) {
        return true;
    }
    // Different major version = potentially incompatible protocol structure
    return !CODE_VERSION_SAME_MAJOR(packet_version, our_version);
}

#ifdef __cplusplus
}
#endif

#endif // VERSION_CONTROL_H