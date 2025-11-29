#include "lib/hashmap_presets.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ---------- STRING KEYS ----------

// djb2 hash for strings
unsigned int string_hash(const void *key) 
{
    const char *s = (const char *)key;
    unsigned long h = 5381;
    int c;
    while ((c = *s++)) {
        h = ((h << 5) + h) + c; // h * 33 + c
    }
    return (unsigned int)h;
}

// string equality
bool string_equals(const void *a, const void *b) 
{
    return strcmp((const char *)a, (const char *)b) == 0;
}

// ---------- INTEGER KEYS ----------

// simple integer hash
unsigned int int_hash(const void *key)
{
    return (unsigned int)(*(const int *)key);
}

// integer equality
bool int_equals(const void *a, const void *b) 
{
    return (*(const int *)a) == (*(const int *)b);
}

// ---------- MAC ADDRESS KEYS (6-byte arrays) ----------

// hash MAC by XORing bytes
unsigned int mac_hash(const void *key) 
{
    const uint8_t *mac = (const uint8_t *)key;
    unsigned int h = 0;
    for (int i = 0; i < 6; i++) {
        h = (h << 5) ^ mac[i]; // mix bits
    }
    return h;
}

// MAC equality
bool mac_equals(const void *a, const void *b) 
{
    return memcmp((const uint8_t *)a, (const uint8_t *)b, 6) == 0;
}

// ---------- GENERIC POINTER KEYS ----------

// identity hash (pointer value itself)
unsigned int ptr_hash(const void *key) 
{
    return (unsigned int)((uintptr_t)key);
}

// pointer equality
bool ptr_equals(const void *a, const void *b) 
{
    return a == b;
}