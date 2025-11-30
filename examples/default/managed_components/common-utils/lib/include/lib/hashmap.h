#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

// Hashmap entry
typedef struct HashMapEntry {
    void *key;
    void *value;
    struct HashMapEntry *next;
} HashMapEntry;

// Hashmap structure
typedef struct {
    HashMapEntry **buckets;
    size_t size; // number of buckets
    unsigned int (*hash_func)(const void *key);
    bool (*equals_func)(const void *a, const void *b);
} HashMap;

// API
void hashmap_init(HashMap *map, size_t size,
                  unsigned int (*hash_func)(const void *),
                  bool (*equals_func)(const void *, const void *));
bool hashmap_put(HashMap *map, void *key, void *value);
void *hashmap_get(const HashMap *map, const void *key);
bool hashmap_remove(HashMap *map, const void *key);
void hashmap_free(HashMap *map);
void hashmap_clear(HashMap *map);
size_t hashmap_size(const HashMap *map);

/**
 * @brief Iterate over all entries in the hashmap and call a callback for each
 * @param map The hashmap to iterate
 * @param callback Function called for each entry (key, value, user_data)
 * @param user_data Optional user data passed to callback
 * @return Number of entries processed
 */
size_t hashmap_foreach(const HashMap *map, 
                       void (*callback)(void *key, void *value, void *user_data),
                       void *user_data);
#ifdef __cplusplus
}
#endif