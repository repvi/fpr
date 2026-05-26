#pragma once

#include <stddef.h>
#include <stdbool.h>

// Simple hashmap implementation for SPROUT peer management

struct hashmap_entry {
    void *key;
    void *value;
    struct hashmap_entry *next;
};

struct hashmap {
    struct hashmap_entry **entries;
    int capacity;
    int size;
    unsigned int (*hash)(const void *key);
    bool (*equals)(const void *key1, const void *key2);
};

typedef struct hashmap hashmap_t;

// Hash function type
typedef unsigned int (*hash_func_t)(const void *key);

// Equality function type
typedef bool (*equals_func_t)(const void *key1, const void *key2);

// Callback type for foreach iteration
typedef void (*hashmap_callback_t)(void *key, void *value, void *user_data);

// Initialize a hashmap
void hashmap_init(hashmap_t *map, int initial_size, hash_func_t hash, equals_func_t equals);

// Free a hashmap
void hashmap_free(hashmap_t *map);

// Get a value from the hashmap
void *hashmap_get(hashmap_t *map, const void *key);

// Put a value into the hashmap
bool hashmap_put(hashmap_t *map, const void *key, void *value);

// Remove a value from the hashmap
bool hashmap_remove(hashmap_t *map, const void *key);

// Get the size of the hashmap
size_t hashmap_size(hashmap_t *map);

// Clear all entries from the hashmap
void hashmap_clear(hashmap_t *map);

// Iterate over all entries in the hashmap
void hashmap_foreach(hashmap_t *map, hashmap_callback_t callback, void *user_data);
