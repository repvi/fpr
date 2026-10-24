/**
 * @file fpr.h
 * @brief FPR (Fast Peer Router) Network Protocol API
 *
 * FPR is a lightweight, secure mesh networking library for ESP32 devices
 * that enables WiFi-like connectivity without requiring traditional WiFi
 * infrastructure. The radio sits behind the injected fpr_transport_t
 * port (ESP-NOW on ESP32, a virtual radio under SIL, an in-memory fake
 * in host tests).
 *
 * Features:
 * - Broadcast-based discovery and unicast connection handshake
 * - Host/Client modes with auto/manual connection flows
 * - WiFi WPA2-style 4-way security handshake
 * - Packet fragmentation for large payloads
 * - Network statistics and diagnostics
 * - Protocol versioning for forward/backward compatibility
 *
 * Modes:
 * - HOST: Central coordinator accepting client connections (Stable)
 * - CLIENT: Connect to hosts and communicate (Stable)
 * - EXTENDER: Mesh relay for multi-hop routing (Under Development)
 * - BROADCAST: Send to all devices in range
 *
 * Usage (see fpr_node.h for the full API):
 * 1. Initialize WiFi (required for ESP-NOW)
 * 2. fpr_create() with device name, init config and callbacks
 * 3. Set mode with fpr_set_mode()
 * 4. Start with fpr_start()
 * 5. Drive periodic work with fpr_poll(), fpr_start_loop_task() and
 *    fpr_start_reconnect_task()
 * 6. Use fpr_send() or fpr_broadcast() to send data
 *
 * @author Alejandro Ramirez
 * @version 1.0.0
 * @date December 2025
 * @since August 2025
 */

#pragma once

#include "fpr/fpr_def.h"
#include "fpr/fpr_version.h"
#include "fpr/fpr_node.h"
#include "fpr/fpr_transport.h"
#include "stdint.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== VERSION API (process-wide — not per-node) ==========

/**
 * @brief Get the current FPR protocol version.
 * @return Protocol version as packed uint32 (major << 16 | minor << 8 | patch).
 */
code_version_t fpr_get_protocol_version(void);

/**
 * @brief Get protocol version as human-readable string.
 * @param buf Buffer to store version string (e.g., "1.0.0").
 * @param buf_size Size of the buffer.
 */
esp_err_t fpr_get_protocol_version_string(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
