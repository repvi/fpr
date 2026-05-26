#pragma once

#include "hashmap.h"
#include <stdint.h>

// MAC address hash function
static inline unsigned int mac_hash(const void *key) {
    const uint8_t *mac = (const uint8_t *)key;
    return (mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5]);
}

// MAC address equality function
static inline bool mac_equals(const void *key1, const void *key2) {
    const uint8_t *mac1 = (const uint8_t *)key1;
    const uint8_t *mac2 = (const uint8_t *)key2;
    return (mac1[0] == mac2[0] && mac1[1] == mac2[1] && mac1[2] == mac2[2] &&
            mac1[3] == mac2[3] && mac1[4] == mac2[4] && mac1[5] == mac2[5]);
}
