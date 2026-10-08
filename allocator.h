#ifndef MEMORY_ALLOCATOR_H
#define MEMORY_ALLOCATOR_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { ALLOCATOR_FIRST_FIT, ALLOCATOR_BEST_FIT } AllocatorStrategy;
typedef struct {
    size_t initial_size; /* Region bytes, rounded up to an OS page. */
    size_t max_size;     /* Hard limit on total mapped region bytes. */
    AllocatorStrategy strategy;
} AllocatorConfig;
typedef struct {
    size_t reserved_bytes, requested_bytes, allocated_capacity;
    size_t free_bytes, largest_free_block, metadata_bytes;
    size_t active_allocations, free_blocks, regions, peak_requested_bytes;
    size_t allocation_count; /* Includes replacement blocks used by realloc. */
    size_t free_count;       /* Includes blocks released by realloc. */
    size_t failed_allocations, invalid_operations;
} AllocatorStats;

/* Defaults: 1 MiB initial region, 64 MiB limit, first fit. */
void allocator_init(void); /* Idempotent; never resets live allocations. */
bool allocator_configure(const AllocatorConfig *config); /* Before first use. */
bool allocator_destroy(void); /* Refuses while allocations remain live. */
void *allocator_alloc(size_t size);
void *allocator_calloc(size_t count, size_t size);
void *allocator_realloc(void *pointer, size_t size);
void allocator_free(void *pointer);
/* All public operations lock the heap. */
bool allocator_validate(void);
bool allocator_get_stats(AllocatorStats *stats);
#endif

