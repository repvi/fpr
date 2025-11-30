#include "lib/hashmap.h"
#include "lib/memcore/mem_pool.h"
#include <stdlib.h>
#include "esp_heap_caps.h"

#define HASHMAP_MEMORY_POOL_SIZE (sizeof(HashMapEntry) * 200)

static uint8_t _hashmap_buffer[HASHMAP_MEMORY_POOL_SIZE];

void hashmap_init(HashMap *map, size_t size,
                  unsigned int (*hash_func)(const void *),
                  bool (*equals_func)(const void *, const void *)) {
    map->size = size;
    map->buckets = calloc(size, sizeof(HashMapEntry *));
    map->hash_func = hash_func;
    map->equals_func = equals_func;
}

bool hashmap_put(HashMap *map, void *key, void *value) {
    unsigned int idx = map->hash_func(key) % map->size;
    HashMapEntry *entry = map->buckets[idx];

    while (entry) {
        if (map->equals_func(entry->key, key)) {
            entry->value = value; // update
            return true;
        }
        entry = entry->next;
    }

    entry = heap_caps_malloc(sizeof(HashMapEntry), MALLOC_CAP_DEFAULT);
    if (entry == NULL) {
        return false; // allocation failed
    }
    entry->key = key;
    entry->value = value;
    entry->next = map->buckets[idx];
    map->buckets[idx] = entry;

    return true;
}

void *hashmap_get(const HashMap *map, const void *key) {
    unsigned int idx = map->hash_func(key) % map->size;
    HashMapEntry *entry = map->buckets[idx];

    while (entry) {
        if (map->equals_func(entry->key, key)) {
            return entry->value;
        }
        entry = entry->next;
    }
    return NULL;
}

bool hashmap_remove(HashMap *map, const void *key) {
    unsigned int idx = map->hash_func(key) % map->size;
    HashMapEntry *entry = map->buckets[idx];
    HashMapEntry *prev = NULL;

    while (entry) {
        if (map->equals_func(entry->key, key)) {
            if (prev) prev->next = entry->next;
            else map->buckets[idx] = entry->next;
            heap_caps_free(entry);
            return true;
        }
        prev = entry;
        entry = entry->next;
    }
    return false;
}

void hashmap_free(HashMap *map) {
    for (size_t i = 0; i < map->size; i++) {
        HashMapEntry *entry = map->buckets[i];
        while (entry) {
            HashMapEntry *next = entry->next;
            heap_caps_free(entry);
            entry = next;
        }
    }
    heap_caps_free(map->buckets);
    map->buckets = NULL;
    map->size = 0;
}

void hashmap_clear(HashMap *map) {
    for (size_t i = 0; i < map->size; i++) {
        HashMapEntry *entry = map->buckets[i];
        while (entry) {
            HashMapEntry *next = entry->next;
            heap_caps_free(entry);
            entry = next;
        }
        map->buckets[i] = NULL;
    }
}

size_t hashmap_size(const HashMap *map)
{
    return map->size;
}

size_t hashmap_foreach(const HashMap *map, 
                       void (*callback)(void *key, void *value, void *user_data),
                       void *user_data) 
{
    if (!map || !callback) {
        return 0;
    }
    
    size_t count = 0;
    for (size_t i = 0; i < map->size; i++) {
        HashMapEntry *entry = map->buckets[i];
        while (entry) {
            callback(entry->key, entry->value, user_data);
            count++;
            entry = entry->next;
        }
    }
    return count;
}