/**
 * @file mem_pool.h
 * @brief Fixed-size memory pool allocator optimized for ESP32 family
 *
 * This header exposes a simple, deterministic fixed-block memory pool API
 * intended for use in constrained embedded contexts (FreeRTOS tasks and
 * optionally ISRs when the pool is allocated in IRAM). The pool provides
 * O(1) alloc/free semantics using a single-linked free list stored inside
 * each block.
 *
 * Key characteristics:
 * - Preallocated contiguous memory region to avoid heap fragmentation
 * - Optional alignment and heap-capability selection via `heap_caps` APIs
 * - Small deterministic runtime overhead suitable for real-time code
 *
 * Usage example:
 * @code
 * mem_pool_t pool;
 * mem_pool_init(&pool, MEM_POOL_NO_ALIGNMENT, 128, 32, MALLOC_CAP_8BIT);
 * void *p = mem_pool_alloc(&pool);
 * mem_pool_free(&pool, p);
 * mem_pool_destroy(&pool);
 * @endcode
 *
 * @note The implementation stores a small header in each block (a `next`
 * pointer). The usable payload begins at the block start — callers should
 * account for header size if necessary or use the block size exactly as
 * provided to the pool initializer.
 *
 * @warning The API performs basic validation but does not provide built-in
 * double-free detection unless enabled in debug builds. Add external
 * synchronization (mutex/portMUX) if multiple tasks will concurrently
 * call `mem_pool_alloc`/`mem_pool_free` on the same pool.
 *
 * @since 2025-05-26
 */

#ifndef MEM_POOL_H
#define MEM_POOL_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lib/genlist.h"
#include "lib/base_macros.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#define MEM_POOL_NO_ALIGNMENT (size_t)(-1)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct memory_block {
    struct memory_block *next;
    /* payload starts immediately after this header (or you can make header size 0) */
} memory_block_t;

enum mem_pool_errors_t {
    MEM_POOL_ERROR_NOT_INITIALIZED = 0,
    MEM_POOL_SUCCESS,
    MEM_POOL_ERROR_INVALID_PARAMS,
    MEM_POOL_ERROR_INVALID_BLOCK,
};

typedef struct {
    void *memory; // remove in the future if updated to not need it
    memory_block_t *memory_free;
    size_t block_size;
    size_t num_blocks;
    size_t free_blocks;
    size_t total_allocated; // for statistics
    size_t alignment;
    struct {
        uintptr_t start;
        uintptr_t end;
    } memory_address;
    portMUX_TYPE lock;
    struct {
        enum mem_pool_errors_t last_error : 4;
        int initialized : 1; // not used currently for anything
        int exhausted : 1;
        int is_static_buffer : 1;
        int reserved : 25;
    } flags;
} mem_pool_t;

/**
 * @brief Initialize a memory pool.
 *
 * @param[in,out] pool Pointer to a preallocated `mem_pool_t` structure to initialize.
 * @param[in] alignment Desired alignment for pool allocation or `MEM_POOL_NO_ALIGNMENT`.
 * @param[in] block_size Size of each block in bytes.
 * @param[in] num_blocks Number of blocks to allocate in the pool.
 * @param[in] region_caps Heap capability flags for `heap_caps_malloc` (e.g. MALLOC_CAP_8BIT).
 *
 * @note On success the pool is ready for `mem_pool_alloc` / `mem_pool_free`.
 *       On failure the pool->memory will be NULL and the flags indicate error state.
 */
void mem_pool_init(mem_pool_t *const pool, size_t alignment, size_t block_size, size_t num_blocks, int region_caps);

void mem_pool_static_init(mem_pool_t *const pool, void* buffer, size_t buffer_size, size_t block_size);

/**
 * @brief Allocate a block from the memory pool.
 *
 * @param[in,out] pool Pointer to an initialized memory pool.
 * @return void* Pointer to a block (usable memory) or NULL if pool is exhausted.
 *
 * @note The returned block must be returned to the same pool with `mem_pool_free`.
 */
void *mem_pool_alloc(mem_pool_t *const pool);

/**
 * @brief Get number of free blocks in the pool.
 *
 * @param[in] pool Pointer to the memory pool.
 * @return size_t Number of free blocks (0 if pool is NULL or exhausted).
 */
size_t mem_pool_get_free_blocks(const mem_pool_t *const pool);

/**
 * @brief Get total blocks managed by the pool.
 *
 * @param[in] pool Pointer to the memory pool.
 * @return size_t Total number of blocks (0 if pool is NULL).
 */
size_t mem_pool_get_total_blocks(const mem_pool_t *const pool);

/**
 * @brief Get number of used blocks in the pool.
 *
 * @param[in] pool Pointer to the memory pool.
 * @return size_t Number of blocks currently allocated/used.
 */
size_t mem_pool_get_used_blocks(const mem_pool_t *const pool);

/**
 * @brief Get block size for this pool.
 *
 * @param[in] pool Pointer to the memory pool.
 * @return size_t Size of a single block in bytes (0 if pool is NULL).
 */
size_t mem_pool_get_block_size(const mem_pool_t *const pool);

/**
 * @brief Return a previously allocated block to the pool.
 *
 * @param[in,out] pool Pointer to the memory pool.
 * @param[in] block Pointer to the block previously returned by `mem_pool_alloc`.
 *
 * @note The function performs basic validation (range/alignment). Passing an invalid
 *       pointer will be ignored or logged depending on build options.
 */
void mem_pool_free(mem_pool_t *const pool, void *block);

/**
 * @brief Destroy the memory pool and free underlying memory.
 *
 * @param[in,out] pool Pointer to the memory pool. After call, the pool is no longer usable.
 */
void mem_pool_destroy(mem_pool_t *const pool);

/**
 * @brief Print pool statistics to the system log (helper for debug).
 *
 * @param[in] pool Pointer to the memory pool.
 */
void mem_pool_print_stats(const mem_pool_t *const pool);

#ifdef __cplusplus
}
#endif

#endif // MEM_POOL_H