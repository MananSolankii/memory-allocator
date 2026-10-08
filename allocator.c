#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif
#include "allocator.h"
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define MIB ((size_t)1024 * 1024)
#define BLOCK_MAGIC UINT32_C(0xA110CA7E)
typedef struct Block Block;
typedef struct Region Region;
struct Block {
    size_t capacity, requested;
    Block *previous, *next;
    Block *free_previous, *free_next;
    uint32_t magic;
    bool is_free;
};
struct Region { size_t size; Region *next; };
static struct {
    Region *regions;
    Block *free_head;
    AllocatorConfig config;
    AllocatorStats counters;
    bool initialized;
} heap;

/* The lock lives for the process lifetime, across heap destruction. */
#ifdef _WIN32
static CRITICAL_SECTION heap_mutex;
static volatile LONG mutex_state;
static void lock_heap(void) {
    if (InterlockedCompareExchange(&mutex_state, 1, 0) == 0) {
        InitializeCriticalSection(&heap_mutex);
        InterlockedExchange(&mutex_state, 2);
    } else {
        while (InterlockedCompareExchange(&mutex_state, 2, 2) != 2) Sleep(0);
    }
    EnterCriticalSection(&heap_mutex);
}
static void unlock_heap(void) { LeaveCriticalSection(&heap_mutex); }
static size_t page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (size_t)info.dwPageSize;
}
static void *map_region(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static void unmap_region(void *address, size_t size) {
    (void)size;
    VirtualFree(address, 0, MEM_RELEASE);
}
#else
static pthread_mutex_t heap_mutex = PTHREAD_MUTEX_INITIALIZER;
static void lock_heap(void) { pthread_mutex_lock(&heap_mutex); }
static void unlock_heap(void) { pthread_mutex_unlock(&heap_mutex); }
static size_t page_size(void) { return (size_t)sysconf(_SC_PAGESIZE); }
static void *map_region(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void unmap_region(void *address, size_t size) { munmap(address, size); }
#endif

static bool round_up(size_t value, size_t alignment, size_t *out) {
    if (!alignment || value > SIZE_MAX - (alignment - 1)) return false;
    *out = (value + alignment - 1) / alignment * alignment;
    return true;
}
static size_t block_header(void) {
    const size_t a = alignof(max_align_t);
    return (sizeof(Block) + a - 1) / a * a;
}
static size_t region_header(void) {
    const size_t a = alignof(max_align_t);
    return (sizeof(Region) + a - 1) / a * a;
}
static Block *first_block(Region *r) {
    return (Block *)((unsigned char *)r + region_header());
}
static void *payload(Block *b) { return (unsigned char *)b + block_header(); }

static void insert_free(Block *b) {
    b->free_previous = NULL;
    b->free_next = heap.free_head;
    if (heap.free_head) heap.free_head->free_previous = b;
    heap.free_head = b;
}
static void remove_free(Block *b) {
    if (b->free_previous) b->free_previous->free_next = b->free_next;
    else heap.free_head = b->free_next;
    if (b->free_next) b->free_next->free_previous = b->free_previous;
    b->free_previous = b->free_next = NULL;
}
static bool add_region(size_t size) {
    Region *r = map_region(size);
    if (!r) return false;
    r->size = size;
    r->next = heap.regions;
    Block *b = first_block(r);
    *b = (Block){0};
    b->capacity = size - region_header() - block_header();
    b->magic = BLOCK_MAGIC;
    b->is_free = true;
    heap.regions = r;
    insert_free(b);
    heap.counters.reserved_bytes += size;
    heap.counters.regions++;
    return true;
}
static bool configure_locked(const AllocatorConfig *config) {
    size_t initial;
    if (heap.initialized || !config ||
        (config->strategy != ALLOCATOR_FIRST_FIT && config->strategy != ALLOCATOR_BEST_FIT) ||
        !config->initial_size ||
        !round_up(config->initial_size, page_size(), &initial) ||
        initial > config->max_size ||
        initial < region_header() + block_header() + alignof(max_align_t)) return false;
    heap.config = *config;
    heap.config.initial_size = initial;
    if (!add_region(initial)) return false;
    heap.initialized = true;
    return true;
}
static bool ensure_initialized(void) {
    const AllocatorConfig defaults = {MIB, 64 * MIB, ALLOCATOR_FIRST_FIT};
    return heap.initialized || configure_locked(&defaults);
}
bool allocator_configure(const AllocatorConfig *config) {
    lock_heap();
    bool ok = configure_locked(config);
    unlock_heap();
    return ok;
}
void allocator_init(void) {
    lock_heap();
    (void)ensure_initialized();
    unlock_heap();
}
static bool grow_heap(size_t capacity) {
    const size_t overhead = region_header() + block_header();
    size_t required;
    if (capacity > SIZE_MAX - overhead ||
        !round_up(capacity + overhead, page_size(), &required)) return false;
    size_t available = heap.config.max_size - heap.counters.reserved_bytes;
    available -= available % page_size();
    if (required > available) return false;
    size_t size = required > heap.config.initial_size ? required : heap.config.initial_size;
    if (size > available) size = available;
    return add_region(size);
}

/* Caller has not put b on the free list. Its next block is free. */
static void absorb_next(Block *b) {
    Block *next = b->next;
    remove_free(next);
    b->capacity += block_header() + next->capacity;
    b->next = next->next;
    if (b->next) b->next->previous = b;
    next->magic = 0;
}
static void split_block(Block *b, size_t capacity) {
    if (b->capacity - capacity < block_header() + alignof(max_align_t)) return;
    Block *tail = (Block *)((unsigned char *)payload(b) + capacity);
    *tail = (Block){0};
    tail->capacity = b->capacity - capacity - block_header();
    tail->previous = b;
    tail->next = b->next;
    tail->magic = BLOCK_MAGIC;
    tail->is_free = true;
    if (tail->next) tail->next->previous = tail;
    b->capacity = capacity;
    b->next = tail;
    /* A shrinking realloc may leave a tail beside an existing free block. */
    if (tail->next && tail->next->is_free) absorb_next(tail);
    insert_free(tail);
}
static void update_peak(void) {
    if (heap.counters.requested_bytes > heap.counters.peak_requested_bytes)
        heap.counters.peak_requested_bytes = heap.counters.requested_bytes;
}
static void *alloc_locked(size_t size) {
    if (!size) return NULL;
    size_t capacity;
    if (!round_up(size, alignof(max_align_t), &capacity) || !ensure_initialized()) {
        heap.counters.failed_allocations++;
        return NULL;
    }
    Block *chosen = NULL;
    for (Block *b = heap.free_head; b; b = b->free_next) {
        if (b->capacity < capacity) continue;
        if (!chosen || b->capacity < chosen->capacity) chosen = b;
        if (heap.config.strategy == ALLOCATOR_FIRST_FIT || b->capacity == capacity) break;
    }
    if (!chosen) {
        if (!grow_heap(capacity)) {
            heap.counters.failed_allocations++;
            return NULL;
        }
        chosen = heap.free_head;
    }
    remove_free(chosen);
    chosen->is_free = false;
    split_block(chosen, capacity);
    chosen->requested = size;
    heap.counters.requested_bytes += size;
    heap.counters.active_allocations++;
    heap.counters.allocation_count++;
    update_peak();
    return payload(chosen);
}
void *allocator_alloc(size_t size) {
    lock_heap();
    void *result = alloc_locked(size);
    unlock_heap();
    return result;
}
void *allocator_calloc(size_t count, size_t size) {
    lock_heap();
    void *result = NULL;
    if (size && count > SIZE_MAX / size) heap.counters.failed_allocations++;
    else {
        result = alloc_locked(count * size);
        if (result) memset(result, 0, count * size);
    }
    unlock_heap();
    return result;
}

/* Equality checks avoid dereferencing pointers supplied by the caller. */
static Block *find_allocation(void *pointer) {
    for (Region *r = heap.regions; r; r = r->next)
        for (Block *b = first_block(r); b; b = b->next)
            if (payload(b) == pointer) return b;
    return NULL;
}
static void free_block(Block *b) {
    heap.counters.requested_bytes -= b->requested;
    heap.counters.active_allocations--;
    heap.counters.free_count++;
    b->requested = 0;
    b->is_free = true;
    if (b->next && b->next->is_free) absorb_next(b);
    if (b->previous && b->previous->is_free) {
        Block *previous = b->previous;
        remove_free(previous);
        previous->capacity += block_header() + b->capacity;
        previous->next = b->next;
        if (previous->next) previous->next->previous = previous;
        b->magic = 0;
        b = previous;
    }
    insert_free(b);
}
void allocator_free(void *pointer) {
    if (!pointer) return;
    lock_heap();
    Block *b = find_allocation(pointer);
    if (!b || b->is_free) heap.counters.invalid_operations++;
    else free_block(b);
    unlock_heap();
}
void *allocator_realloc(void *pointer, size_t size) {
    lock_heap();
    void *result = NULL;
    if (!pointer) result = alloc_locked(size);
    else {
        Block *b = find_allocation(pointer);
        size_t capacity;
        if (!b || b->is_free) heap.counters.invalid_operations++;
        else if (!size) free_block(b);
        else if (!round_up(size, alignof(max_align_t), &capacity))
            heap.counters.failed_allocations++;
        else {
            if (capacity > b->capacity && b->next && b->next->is_free &&
                capacity <= b->capacity + block_header() + b->next->capacity)
                absorb_next(b);
            if (capacity <= b->capacity) {
                heap.counters.requested_bytes -= b->requested;
                heap.counters.requested_bytes += size;
                b->requested = size;
                split_block(b, capacity);
                update_peak();
                result = pointer;
            } else {
                result = alloc_locked(size);
                if (result) {
                    memcpy(result, pointer, b->requested);
                    free_block(b);
                }
            }
        }
    }
    unlock_heap();
    return result;
}

/* Called only after the physical block chains have been validated. */
static bool known_block(Block *candidate) {
    for (Region *r = heap.regions; r; r = r->next)
        for (Block *b = first_block(r); b; b = b->next)
            if (b == candidate) return true;
    return false;
}
static bool validate_locked(void) {
    size_t regions = 0, reserved = 0, free_blocks = 0, active = 0, requested = 0;
    for (Region *r = heap.regions; r; r = r->next) {
        if (++regions > heap.counters.regions ||
            r->size < region_header() + block_header() + alignof(max_align_t) ||
            r->size % page_size() || reserved > heap.counters.reserved_bytes ||
            r->size > heap.counters.reserved_bytes - reserved) return false;
        reserved += r->size;
        size_t offset = region_header();
        Block *previous = NULL;
        while (offset < r->size) {
            if (r->size - offset < block_header()) return false;
            Block *b = (Block *)((unsigned char *)r + offset);
            if ((uintptr_t)b % alignof(max_align_t) || b->magic != BLOCK_MAGIC ||
                b->previous != previous || b->capacity < alignof(max_align_t) ||
                b->capacity % alignof(max_align_t) ||
                b->capacity > r->size - offset - block_header()) return false;
            size_t end = offset + block_header() + b->capacity;
            Block *expected_next = end == r->size ? NULL :
                (Block *)((unsigned char *)r + end);
            if (b->next != expected_next) return false;
            if (b->is_free) {
                if (b->requested || (previous && previous->is_free)) return false;
                free_blocks++;
            } else {
                if (!b->requested || b->requested > b->capacity ||
                    b->free_previous || b->free_next) return false;
                active++;
                requested += b->requested;
            }
            previous = b;
            offset = end;
        }
    }
    size_t listed = 0;
    Block *previous = NULL;
    for (Block *b = heap.free_head; b; b = b->free_next) {
        if (++listed > free_blocks || !known_block(b) || !b->is_free ||
            b->free_previous != previous) return false;
        previous = b;
    }
    return listed == free_blocks && regions == heap.counters.regions &&
        reserved == heap.counters.reserved_bytes &&
        active == heap.counters.active_allocations && requested == heap.counters.requested_bytes;
}
bool allocator_validate(void) {
    lock_heap();
    bool valid = validate_locked();
    unlock_heap();
    return valid;
}
bool allocator_get_stats(AllocatorStats *stats) {
    if (!stats) return false;
    lock_heap();
    bool valid = validate_locked();
    if (valid) {
        *stats = heap.counters;
        for (Region *r = heap.regions; r; r = r->next) {
            stats->metadata_bytes += region_header();
            for (Block *b = first_block(r); b; b = b->next) {
                stats->metadata_bytes += block_header();
                if (b->is_free) {
                    stats->free_blocks++;
                    stats->free_bytes += b->capacity;
                    if (b->capacity > stats->largest_free_block)
                        stats->largest_free_block = b->capacity;
                } else stats->allocated_capacity += b->capacity;
            }
        }
    }
    unlock_heap();
    return valid;
}
bool allocator_destroy(void) {
    lock_heap();
    bool ok = heap.counters.active_allocations == 0;
    if (ok) {
        Region *r = heap.regions;
        while (r) {
            Region *next = r->next;
            unmap_region(r, r->size);
            r = next;
        }
        memset(&heap, 0, sizeof(heap));
    }
    unlock_heap();
    return ok;
}

