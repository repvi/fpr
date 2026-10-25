#ifndef FPR_VERSION_H
#define FPR_VERSION_H

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

// Major version comparison (for breaking changes)
#define CODE_VERSION_SAME_MAJOR(v1, v2) (CODE_VERSION_MAJOR(v1) == CODE_VERSION_MAJOR(v2))

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif // VERSION_CONTROL_H