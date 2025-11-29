#ifndef MEMCORE_H

#include "esp_heap_caps.h"
#include <stddef.h>

struct memcore_blocks;
typedef struct memcore_blocks memcore_handler_t;

#ifdef __cplusplus
extern "C" {
#endif

memcore_handler_t* memcore_create();

void *memcore_malloc(memcore_handler_t *memcore, size_t size);

void memcore_free(memcore_handler_t *memcore, void *ptr);

void memcore_deallocate_all(memcore_handler_t *memcore);

void memcore_print_allocated_info(memcore_handler_t *memcore);

int memcore_get_total_blocks(memcore_handler_t *memcore);

void *imalloc(size_t size) __attribute__((malloc));

void *icalloc(size_t size);

void *irealloc(void *ptr, size_t size);

void ifree(void *ptr);

#ifdef __cplusplus
}
#endif

#endif // MEMCORE_H