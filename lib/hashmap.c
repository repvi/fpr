#include "hashmap.h"
#include <stdlib.h>
#include <string.h>

#define HASHMAP_INITIAL_CAPACITY 32
#define HASHMAP_LOAD_FACTOR 0.75

void hashmap_init(hashmap_t *map, int initial_size, hash_func_t hash, equals_func_t equals) {
    int capacity = initial_size > 0 ? initial_size : HASHMAP_INITIAL_CAPACITY;
    map->entries = calloc(capacity, sizeof(struct hashmap_entry *));
    map->capacity = capacity;
    map->size = 0;
    map->hash = hash;
    map->equals = equals;
}

void hashmap_free(hashmap_t *map) {
    if (!map) return;
    
    for (int i = 0; i < map->capacity; i++) {
        struct hashmap_entry *entry = map->entries[i];
        while (entry) {
            struct hashmap_entry *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    
    free(map->entries);
    map->entries = NULL;
    map->capacity = 0;
    map->size = 0;
}

void *hashmap_get(hashmap_t *map, const void *key) {
    if (!map || !key) return NULL;
    
    unsigned int index = map->hash(key) % map->capacity;
    struct hashmap_entry *entry = map->entries[index];
    
    while (entry) {
        if (map->equals(entry->key, key)) {
            return entry->value;
        }
        entry = entry->next;
    }
    
    return NULL;
}

bool hashmap_put(hashmap_t *map, const void *key, void *value) {
    if (!map || !key) return false;
    
    unsigned int index = map->hash(key) % map->capacity;
    struct hashmap_entry *entry = map->entries[index];
    
    // Check if key already exists
    while (entry) {
        if (map->equals(entry->key, key)) {
            entry->value = value;
            return true;
        }
        entry = entry->next;
    }
    
    // Create new entry - store the key pointer (not a copy)
    struct hashmap_entry *new_entry = malloc(sizeof(struct hashmap_entry));
    if (!new_entry) return false;
    
    new_entry->key = (void *)key;
    new_entry->value = value;
    new_entry->next = map->entries[index];
    map->entries[index] = new_entry;
    map->size++;
    
    return true;
}

bool hashmap_remove(hashmap_t *map, const void *key) {
    if (!map || !key) return false;
    
    unsigned int index = map->hash(key) % map->capacity;
    struct hashmap_entry *entry = map->entries[index];
    struct hashmap_entry *prev = NULL;
    
    while (entry) {
        if (map->equals(entry->key, key)) {
            if (prev) {
                prev->next = entry->next;
            } else {
                map->entries[index] = entry->next;
            }
            free(entry);
            map->size--;
            return true;
        }
        prev = entry;
        entry = entry->next;
    }
    
    return false;
}

size_t hashmap_size(hashmap_t *map) {
    return map ? map->size : 0;
}

void hashmap_clear(hashmap_t *map) {
    if (!map) return;
    
    for (int i = 0; i < map->capacity; i++) {
        struct hashmap_entry *entry = map->entries[i];
        while (entry) {
            struct hashmap_entry *next = entry->next;
            free(entry);
            entry = next;
        }
        map->entries[i] = NULL;
    }
    
    map->size = 0;
}

void hashmap_foreach(hashmap_t *map, hashmap_callback_t callback, void *user_data) {
    if (!map || !callback) return;
    
    for (int i = 0; i < map->capacity; i++) {
        struct hashmap_entry *entry = map->entries[i];
        while (entry) {
            callback(entry->key, entry->value, user_data);
            entry = entry->next;
        }
    }
}
