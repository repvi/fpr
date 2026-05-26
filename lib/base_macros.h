#pragma once

// Common macros and utility definitions for SPROUT

#define FPR_CONNECT_NAME_SIZE 32
#define FPR_DEFAULT_MAX_HOPS 10
#define FPR_KEY_SIZE 16
#define FPR_QUEUE_LENGTH 20

// SPROUT-prefixed versions for consistency
#define SPROUT_CONNECT_NAME_SIZE 32
#define SPROUT_DEFAULT_MAX_HOPS 10
#define SPROUT_BROADCAST_ADDRESS {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
#define SPROUT_QUEUE_LENGTH 20
#define US_TO_MS(us) ((us) / 1000)

