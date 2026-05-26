#pragma once

#include <stdint.h>

// Version control macros and types for SPROUT protocol versioning

// Version type - packed as uint32: (major << 16 | minor << 8 | patch)
typedef uint32_t code_version_t;

// Create a version number from major, minor, patch
#define CODE_VERSION(major, minor, patch) \
    (((major) & 0xFF) << 16 | ((minor) & 0xFF) << 8 | ((patch) & 0xFF))

// Extract major version
#define CODE_VERSION_MAJOR(version) \
    (((version) >> 16) & 0xFF)

// Extract minor version
#define CODE_VERSION_MINOR(version) \
    (((version) >> 8) & 0xFF)

// Extract patch version
#define CODE_VERSION_PATCH(version) \
    ((version) & 0xFF)

// Check if version is at least the target version
#define CODE_VERSION_AT_LEAST(v, target) \
    ((v) >= (target))

// Check if two versions have the same major version
#define CODE_VERSION_SAME_MAJOR(v1, v2) \
    (CODE_VERSION_MAJOR(v1) == CODE_VERSION_MAJOR(v2))

// Check if v1 is less than v2
#define CODE_VERSION_LESS_THAN(v1, v2) \
    ((v1) < (v2))
