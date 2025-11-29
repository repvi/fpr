#include "lib/memcore/memcore.h"
#include "lib/genlist.h"
#include "esp_log.h"

static const char *TAG = "Memcore";

#define ALIGN_4  __attribute__((aligned(4)))
#define ALIGN_8  __attribute__((aligned(8)))
#define ALIGN_16 __attribute__((aligned(16)))

#define ALIGN_SIZE(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_4_SIZE(x)  ALIGN_SIZE(x, 4)
#define ALIGN_8_SIZE(x)  ALIGN_SIZE(x, 8)
#define ALIGN_16_SIZE(x) ALIGN_SIZE(x, 16)

#define MEMCORE_LIST_OFFSET_ALLOC(size) (sizeof(struct memcore_list) + size)

struct memcore_list {
    struct list_head list;
    void *ptr;
} ALIGN_4;

struct memcore_blocks {
    struct memcore_list mem_list;
    unsigned int total_blocks;
} ALIGN_4;

#define MEMCORE_CAST (struct memcore_blocks *)

memcore_handler_t *memcore_create()
{
    struct memcore_blocks *memcore = MEMCORE_CAST heap_caps_malloc(sizeof(struct memcore_blocks), MALLOC_CAP_32BIT | MALLOC_CAP_DMA);
    if (memcore) {
        INIT_LIST_HEAD(&memcore->mem_list.list);
        memcore->total_blocks = 0;
    }
    return memcore;
}

void *memcore_malloc(memcore_handler_t *memcore, size_t size)
{
    struct memcore_blocks *blocks = MEMCORE_CAST memcore;
    size_t total_size = MEMCORE_LIST_OFFSET_ALLOC(size);
    struct memcore_list *new_node = (struct memcore_list *)imalloc(total_size);
    if (new_node != NULL) {
        list_add_tail(&new_node->list, &blocks->mem_list.list);
        blocks->total_blocks++;
        new_node->ptr = (void *)(new_node + 1);
        return new_node->ptr;
    }
    return NULL;
}

void *memcore_calloc(memcore_handler_t *memcore, size_t size)
{
    size_t total_size = MEMCORE_LIST_OFFSET_ALLOC(size);
    struct memcore_list *new_node = (struct memcore_list *)icalloc(total_size);
    if (new_node) {
        list_add_tail(&new_node->list, &memcore->mem_list.list);
        memcore->total_blocks++;
        new_node->ptr = (void *)((uint8_t *)new_node + sizeof(struct memcore_list));
        return new_node->ptr;
    }
    else {
        return NULL;
    }
}

void memcore_free(memcore_handler_t *memcore, void *ptr)
{
    struct memcore_blocks *blocks = MEMCORE_CAST memcore;
    if (blocks->total_blocks > 0) {
        blocks->total_blocks--;
        struct memcore_list *node = (struct memcore_list *)container_of((uint8_t *)ptr, struct memcore_list, ptr);
        list_del(&node->list);
        heap_caps_free(node);
    }
}

void memcore_deallocate_all(memcore_handler_t *memcore)
{
    struct memcore_blocks *blocks = MEMCORE_CAST memcore;
    struct memcore_list *pos, *n;
    list_for_each_entry_safe(pos, n, &blocks->mem_list.list, list) {
        pos->ptr = NULL;
        list_del(&pos->list);
        heap_caps_free(pos);
    }
    blocks->total_blocks = 0;
}

void memcore_print_allocated_info(memcore_handler_t *memcore)
{
    struct memcore_blocks *blocks = MEMCORE_CAST memcore;
    if (blocks->total_blocks > 0) {
        ESP_LOGW(TAG, "Memory allocated: %u blocks still allocated.", blocks->total_blocks);
    }
    else {
        ESP_LOGI(TAG, "No memory allocated.");
    }
}

int memcore_get_total_blocks(memcore_handler_t *memcore)
{
    return memcore->total_blocks;
}

void *imalloc(size_t size)
{
    size_t total = ALIGN_4_SIZE(size); // for safety
    return heap_caps_malloc(total, MALLOC_CAP_32BIT | MALLOC_CAP_DMA);
}

void *icalloc(size_t size)
{
    size_t total = ALIGN_4_SIZE(size); // for safety
    return heap_caps_calloc(1, total, MALLOC_CAP_32BIT | MALLOC_CAP_DMA);
}

void *irealloc(void *ptr, size_t size)
{
    size_t total = ALIGN_4_SIZE(size); // for safety
    return heap_caps_realloc(ptr, total, MALLOC_CAP_32BIT | MALLOC_CAP_DMA);
}

void ifree(void *ptr)
{
    heap_caps_free(ptr);
}