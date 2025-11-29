#include "lib/memcore/mem_pool.h"
#include "lib/memcore/memcore.h"
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#define SAFE_ACCESS(_lockP, variable) ({ \
    portENTER_CRITICAL((_lockP)); \
    typeof(variable) _val = (variable); \
    portEXIT_CRITICAL((_lockP)); \
    _val; \
})

static inline void set_mem_block_list(void *buffer, size_t num_blocks, size_t block_size) 
{
    memory_block_t *blk = (memory_block_t *)buffer;
    for (size_t i = 0; i < num_blocks; ++i) {
        memory_block_t *next = (i + 1 < num_blocks) ? (memory_block_t *)((uint8_t *)blk + block_size) : NULL;
        blk->next = next;
        blk = next;
    }
}

void mem_pool_init(mem_pool_t *const pool, size_t alignment, size_t block_size, size_t num_blocks, int region_caps) 
{
    size_t total_size = block_size * num_blocks;
    if (alignment != MEM_POOL_NO_ALIGNMENT) {
        pool->memory = heap_caps_aligned_alloc(alignment, total_size, region_caps);
    }
    else {
        pool->memory = heap_caps_malloc(total_size, region_caps);
    }

    if (pool->memory != NULL) {
        pool->block_size = block_size;
        pool->num_blocks = num_blocks;
        pool->free_blocks = num_blocks;
        pool->memory_free = (memory_block_t *)pool->memory;
        pool->memory_address.start = (uintptr_t)(pool->memory);
        pool->memory_address.end = (uintptr_t)((uint8_t *)pool->memory + total_size);
        set_mem_block_list(pool->memory, num_blocks, block_size);

        pool->alignment = alignment;
        pool->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

        pool->flags.initialized = 1;
        pool->flags.exhausted = 0;
        pool->flags.last_error = MEM_POOL_SUCCESS;
    }
    else {
        pool->memory_free = NULL;
        pool->block_size = 0;
        pool->num_blocks = 0;
        pool->free_blocks = 0;
        pool->memory_address.start = 0;
        pool->memory_address.end = 0;
        pool->alignment = 0;

        pool->flags.initialized = 0;
        pool->flags.exhausted = 0;
        pool->flags.last_error = MEM_POOL_ERROR_NOT_INITIALIZED;
    }

    pool->total_allocated = 0;
    pool->flags.is_static_buffer = 0;
}

void mem_pool_static_init(mem_pool_t *const pool, void* buffer, size_t buffer_size, size_t block_size) 
{
    if (buffer) {
        size_t num_blocks = buffer_size / block_size;
        pool->alignment = MEM_POOL_NO_ALIGNMENT;
        pool->block_size = block_size;
        pool->free_blocks = num_blocks;
        pool->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
        pool->memory = buffer;
        pool->memory_free = (memory_block_t *)buffer;
        pool->memory_address.start = (uintptr_t)buffer;
        pool->memory_address.end = (uintptr_t)buffer + (uintptr_t)buffer_size;
        set_mem_block_list(pool->memory, num_blocks, block_size);
        pool->flags.initialized = 1;
        pool->flags.is_static_buffer = 1;
        pool->num_blocks = num_blocks;
    }
    else {
        pool->flags.initialized = 0;
    }
}

void *mem_pool_alloc(mem_pool_t *const pool)
{   
    portENTER_CRITICAL(&pool->lock);
    memory_block_t *chunk = pool->memory_free;
    if (chunk) {
        pool->memory_free = chunk->next;
        if (pool->free_blocks > 0) pool->free_blocks--;
        pool->total_allocated++;
    }
    else {
        pool->flags.exhausted = 1;
    }
    portEXIT_CRITICAL(&pool->lock);
    return (void *)chunk;
}

size_t mem_pool_get_free_blocks(const mem_pool_t *const pool) 
{
    return SAFE_ACCESS(&pool->lock, pool->free_blocks);
}

size_t mem_pool_get_total_blocks(const mem_pool_t *const pool) 
{
    return SAFE_ACCESS(&pool->lock, pool->num_blocks);
}

size_t mem_pool_get_used_blocks(const mem_pool_t *const pool) 
{
    portENTER_CRITICAL(&pool->lock);
    size_t total_blocks = pool->num_blocks;
    size_t free_blocks = pool->free_blocks;
    portEXIT_CRITICAL(&pool->lock); 
    return (total_blocks - free_blocks);
}

size_t mem_pool_get_block_size(const mem_pool_t *const pool) 
{
    return SAFE_ACCESS(&pool->lock, pool->block_size);
}

static inline bool is_pointer_in_pool(const mem_pool_t *const pool, const void *const ptr)
{
    return (pool->memory_address.start <= (uintptr_t)ptr
            && (uintptr_t)ptr < pool->memory_address.end);
}

void mem_pool_free(mem_pool_t *const pool, void *block)
{
    portENTER_CRITICAL(&pool->lock);
    if (is_pointer_in_pool(pool, block)) {
        memory_block_t *chunk = (memory_block_t *)block; // might need cast
        chunk->next = pool->memory_free;
        pool->memory_free = chunk;
        pool->free_blocks++;
        pool->flags.exhausted = 0;
        pool->flags.last_error = MEM_POOL_SUCCESS;
    }
    else {
        pool->flags.last_error = MEM_POOL_ERROR_INVALID_BLOCK;
    }
    portEXIT_CRITICAL(&pool->lock);
}

void mem_pool_destroy(mem_pool_t *const pool)
{
    portENTER_CRITICAL(&pool->lock);
    if (pool->memory) {
        if (pool->flags.is_static_buffer == 0) {
            heap_caps_free(pool->memory);
        }
        pool->memory = NULL;
        pool->memory_free = NULL;
    }
    pool->block_size = 0;
    pool->num_blocks = 0;
    pool->free_blocks = 0;
    pool->memory_address.start = 0;
    pool->memory_address.end = 0;
    portEXIT_CRITICAL(&pool->lock);
}

static void print_memory_region(uintptr_t start, uintptr_t end) 
{
    printf("Memory Region: Start = 0x%" PRIxPTR ", End = 0x%" PRIxPTR "\n", start, end);
    if (esp_ptr_in_dram((void *)start) && esp_ptr_in_dram((void *)end)) {
        printf("  Located in DRAM\n");
    }
    else if (esp_ptr_in_iram((void *)start) && esp_ptr_in_iram((void *)end)) {
        printf("  Located in IRAM\n");
    }
    else {
        printf("  Located in Unknown Memory Region\n");
    }
}

void mem_pool_print_stats(const mem_pool_t *const pool) 
{
    portENTER_CRITICAL(&pool->lock);
    size_t total_blocks = pool->num_blocks;
    size_t free_blocks = pool->free_blocks;
    size_t used_blocks = total_blocks - free_blocks;
    size_t block_size = pool->block_size;
    size_t total_allocated = pool->total_allocated;
    size_t alignment = pool->alignment;
        
    int exhausted = pool->flags.exhausted;
    enum mem_pool_errors_t last_error = pool->flags.last_error;

    uintptr_t start_addr = pool->memory_address.start;
    uintptr_t end_addr = pool->memory_address.end;
    portEXIT_CRITICAL(&pool->lock);

    printf("Memory Pool Stats:\n");
    printf("  Block Size: %" PRId32 " bytes\n", (int32_t)block_size);
    printf("  Total Blocks: %" PRId32 "\n", (int32_t)total_blocks);
    printf("  Used Blocks: %" PRId32 "\n", (int32_t)used_blocks);
    printf("  Free Blocks: %" PRId32 "\n", (int32_t)free_blocks);
    printf("  Total Allocated Blocks: %" PRId32 "\n", (int32_t)total_allocated);
    printf("  Alignment: %" PRId32 " bytes\n", (int32_t)alignment);
    print_memory_region(start_addr, end_addr);
    
    if (last_error != MEM_POOL_SUCCESS) {
        printf("  Last Error: %d\n", (int)last_error);
    }
    if (exhausted){
        printf("  Pool is currently exhausted!\n");
    }
}