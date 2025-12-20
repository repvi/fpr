#pragma once

/**
 * @file fpr_lts.h
 * @brief FPR Long-Term Support (LTS) version definitions
 * 
 * This header defines version constants and compatibility boundaries
 * for the FPR protocol. Use these to determine which features are
 * available and which legacy handlers are needed.
 * 
 * @version 1.0.0
 * @date December 2024
 * 
 * VERSION POLICY:
 * - Major version change: Breaking protocol changes (packet structure)
 * - Minor version change: New features, backward compatible
 * - Patch version change: Bug fixes only
 * 
 * COMPATIBILITY RULES:
 * - Same major version: Fully compatible
 * - Different major version: Requires legacy handler
 * - Version 0: Pre-versioning era (legacy)
 * 
 * LTS SUPPORT:
 * - Version 1.0.0 is the first LTS (Long-Term Support) release
 * - LTS versions receive security and critical bug fixes for 2+ years
 * - Host and Client modes are fully functional and production-ready
 * - Extender mode is under active development for version 1.1.0
 */

#include "lib/version_control.h"
#include "esp_now.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========== CURRENT VERSION ==========

/** Current FPR protocol version (1.0.0 - First LTS Release) */
#define FPR_PROTOCOL_VERSION        CODE_VERSION(1, 0, 0)

/** Minimum version we support receiving from */
#define FPR_MIN_SUPPORTED_VERSION   CODE_VERSION(1, 0, 0)

// ========== VERSION HISTORY ==========

/** Version 0 - Pre-versioning era (legacy devices) */
#define FPR_VERSION_LEGACY          CODE_VERSION(0, 0, 0)

/** Version 1.0.0 - First LTS release with host/client modes, security handshake, fragmentation */
#define FPR_VERSION_1_0_0           CODE_VERSION(1, 0, 0)

// Future versions:
// #define FPR_VERSION_1_1_0        CODE_VERSION(1, 1, 0)  // Extender mode completion
// #define FPR_VERSION_1_5_0        CODE_VERSION(1, 5, 0)  // Enterprise security
// #define FPR_VERSION_2_0_0        CODE_VERSION(2, 0, 0)  // Breaking change

// ========== FEATURE FLAGS BY VERSION ==========

/** Check if version supports fragmented packets */
#define FPR_SUPPORTS_FRAGMENTATION(v)  CODE_VERSION_AT_LEAST(v, FPR_VERSION_1_0_0)

/** Check if version supports mesh routing */
#define FPR_SUPPORTS_MESH_ROUTING(v)   CODE_VERSION_AT_LEAST(v, FPR_VERSION_1_0_0)

/** Check if version has protocol versioning */
#define FPR_HAS_VERSIONING(v)          ((v) != FPR_VERSION_LEGACY)

// ========== COMPATIBILITY CHECKS ==========

/**
 * @brief Check if a remote version is compatible with current protocol
 * @param remote_version Version reported by remote device
 * @return true if we can communicate, false if incompatible
 */
static inline bool fpr_version_is_compatible(code_version_t remote_version)
{
    return CODE_VERSION_AT_LEAST(remote_version, FPR_MIN_SUPPORTED_VERSION);
}

/**
 * @brief Check if a remote version needs special handling (older or newer)
 * @param remote_version Version reported by remote device
 * @return true if current version (no special handling), false if needs legacy/new handler
 */
static inline bool fpr_version_is_current(code_version_t remote_version)
{
    // Same major version = compatible, no special handling needed
    return CODE_VERSION_SAME_MAJOR(remote_version, FPR_PROTOCOL_VERSION);
}

/**
 * @brief Check if a remote version is older and needs legacy handling
 * @param remote_version Version reported by remote device
 * @return true if older version that needs legacy handler
 */
static inline bool fpr_version_needs_legacy_handler(code_version_t remote_version)
{
    // Version 0 or older major version needs legacy handling
    return (remote_version == 0) || 
           CODE_VERSION_LESS_THAN(remote_version, CODE_VERSION(CODE_VERSION_MAJOR(FPR_PROTOCOL_VERSION), 0, 0));
}

/**
 * @brief Check if a remote version is newer (future compatibility)
 * @param remote_version Version reported by remote device
 * @return true if newer major version
 */
static inline bool fpr_version_needs_newer_handler(code_version_t remote_version)
{
    return CODE_VERSION_MAJOR(remote_version) > CODE_VERSION_MAJOR(FPR_PROTOCOL_VERSION);
}

/**
 * @brief Get the current protocol version
 * @return Current FPR protocol version
 */
static inline code_version_t fpr_get_current_version(void)
{
    return FPR_PROTOCOL_VERSION;
}

// ========== LTS UTILITY FUNCTIONS ==========

/**
 * @brief Convert version to human-readable string
 * @param version Version to convert
 * @param buf Buffer to store string (recommend at least 16 bytes)
 * @param buf_size Size of buffer
 */
void fpr_lts_version_to_string(code_version_t version, char *buf, size_t buf_size);

/**
 * @brief Log compatibility information for debugging
 * @param remote_version Version received from remote device
 */
void fpr_lts_log_compatibility(code_version_t remote_version);

/**
 * @brief Get the minimum supported protocol version
 * @return Minimum supported version for communication
 */
code_version_t fpr_lts_get_min_supported_version(void);

/**
 * @brief Check if a specific feature is supported by a version
 * @param version Protocol version to check
 * @param feature Feature name: "fragmentation", "mesh_routing", "versioning"
 * @return true if the version supports the feature
 */
bool fpr_lts_supports_feature(code_version_t version, const char *feature);

#ifdef __cplusplus
}
#endif