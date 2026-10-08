#include "allocator.h"
#include <stdint.h>
#include <stdio.h>
#include <stdalign.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define MIB ((size_t)1024 * 1024)
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return false; \
} } while (0)

static bool start_heap(size_t initial, size_t maximum, AllocatorStrategy strategy) {
    CHECK(allocator_destroy());
    AllocatorConfig config = {initial, maximum, strategy};
    CHECK(allocator_configure(&config));
    CHECK(allocator_validate());
    return true;
}
static bool matches(const unsigned char *p, size_t size, unsigned char value) {
    for (size_t i = 0; i < size; i++) if (p[i] != value) return false;
    return true;
}
static bool configuration(void) {
    CHECK(allocator_destroy());
    CHECK(allocator_validate());
    CHECK(!allocator_configure(NULL));
    AllocatorConfig bad = {0, MIB, ALLOCATOR_FIRST_FIT};
    CHECK(!allocator_configure(&bad));
    bad.initial_size = MIB * 2;
    CHECK(!allocator_configure(&bad));
    bad.initial_size = SIZE_MAX;
    CHECK(!allocator_configure(&bad));
    bad.initial_size = MIB;
    bad.strategy = (AllocatorStrategy)99;
    CHECK(!allocator_configure(&bad));
    CHECK(start_heap(MIB, MIB, ALLOCATOR_FIRST_FIT));
    char *p = allocator_alloc(32);
    CHECK(p != NULL);
    strcpy(p, "preserved");
    allocator_init();
    CHECK(strcmp(p, "preserved") == 0);
    CHECK(!allocator_destroy());
    AllocatorConfig config = {MIB, MIB, ALLOCATOR_FIRST_FIT};
    CHECK(!allocator_configure(&config));
    allocator_free(p);
    CHECK(allocator_destroy());
    return true;
}
static bool alignment_and_overflow(void) {
    CHECK(start_heap(MIB, MIB, ALLOCATOR_FIRST_FIT));
    CHECK(allocator_alloc(0) == NULL);
    CHECK(allocator_calloc(0, 8) == NULL);
    CHECK(allocator_calloc(8, 0) == NULL);
    CHECK(allocator_alloc(SIZE_MAX) == NULL);
    CHECK(allocator_calloc(SIZE_MAX, 2) == NULL);
    CHECK(allocator_calloc(SIZE_MAX / 2 + 1, 2) == NULL);
    void *pointers[128];
    for (size_t i = 0; i < 128; i++) {
        pointers[i] = allocator_alloc(i + 1);
        CHECK(pointers[i] && (uintptr_t)pointers[i] % alignof(max_align_t) == 0);
        memset(pointers[i], (int)i, i + 1);
    }
    for (size_t i = 0; i < 128; i++) {
        CHECK(matches(pointers[i], i + 1, (unsigned char)i));
        allocator_free(pointers[i]);
    }
    allocator_free(NULL);
    AllocatorStats s;
    CHECK(allocator_get_stats(&s));
    CHECK(s.failed_allocations == 3 && s.active_allocations == 0);
    CHECK(s.free_blocks == 1);
    CHECK(allocator_destroy());
    return true;
}
static bool zero_initialization(void) {
    CHECK(start_heap(MIB, MIB, ALLOCATOR_FIRST_FIT));
    unsigned char *p = allocator_alloc(4096);
    CHECK(p);
    memset(p, 0xff, 4096);
    allocator_free(p);
    p = allocator_calloc(1024, 4);
    CHECK(p && matches(p, 4096, 0));
    allocator_free(p);
    CHECK(allocator_validate());
    CHECK(allocator_destroy());
    return true;
}
static bool coalescing_and_statistics(void) {
    CHECK(start_heap(MIB, MIB, ALLOCATOR_FIRST_FIT));
    AllocatorStats initial, s;
    CHECK(allocator_get_stats(&initial));
    void *a = allocator_alloc(100), *b = allocator_alloc(200), *c = allocator_alloc(300);
    CHECK(a && b && c);
    CHECK(allocator_get_stats(&s));
    CHECK(s.active_allocations == 3 && s.requested_bytes == 600);
    CHECK(s.peak_requested_bytes == 600 && s.allocation_count == 3);
    CHECK(s.allocated_capacity >= s.requested_bytes);
    CHECK(s.reserved_bytes == s.free_bytes + s.allocated_capacity + s.metadata_bytes);
    allocator_free(a);
    allocator_free(c);
    CHECK(allocator_validate());
    allocator_free(b); /* Merges both neighbors. */
    CHECK(allocator_get_stats(&s));
    CHECK(s.free_blocks == 1 && s.free_bytes == initial.free_bytes);
    CHECK(s.free_count == 3 && s.requested_bytes == 0 && s.active_allocations == 0);
    CHECK(s.peak_requested_bytes == 600);
    CHECK(allocator_destroy());
    return true;
}
static bool invalid_operations(void) {
    CHECK(start_heap(MIB, MIB, ALLOCATOR_FIRST_FIT));
    unsigned char *p = allocator_alloc(64);
    CHECK(p);
    int outside = 42;
    allocator_free(&outside);
    allocator_free(p + 1);
    CHECK(allocator_realloc(&outside, 8) == NULL);
    allocator_free(p);
    allocator_free(p);
    AllocatorStats s;
    CHECK(allocator_get_stats(&s));
    CHECK(s.invalid_operations == 4 && s.active_allocations == 0);
    CHECK(allocator_destroy());
    return true;
}
static bool resizing(void) {
    CHECK(start_heap(MIB, MIB, ALLOCATOR_FIRST_FIT));
    unsigned char *p = allocator_alloc(64);
    void *neighbor = allocator_alloc(256), *guard = allocator_alloc(64);
    CHECK(p && neighbor && guard);
    memset(p, 0x5a, 64);
    allocator_free(neighbor);
    unsigned char *grown = allocator_realloc(p, 200);
    CHECK(grown == p && matches(grown, 64, 0x5a));
    memset(grown, 0x5a, 200);
    unsigned char *shrunk = allocator_realloc(grown, 32);
    CHECK(shrunk == grown && matches(shrunk, 32, 0x5a));
    CHECK(allocator_validate());
    unsigned char *moved = allocator_realloc(shrunk, 4096);
    CHECK(moved && moved != shrunk && matches(moved, 32, 0x5a));
    memset(moved, 0x33, 4096);
    CHECK(allocator_realloc(moved, SIZE_MAX) == NULL);
    CHECK(matches(moved, 4096, 0x33));
    CHECK(allocator_realloc(moved, MIB * 2) == NULL);
    CHECK(matches(moved, 4096, 0x33));
    CHECK(allocator_realloc(moved, 0) == NULL);
    allocator_free(guard);
    void *from_null = allocator_realloc(NULL, 64);
    CHECK(from_null);
    allocator_free(from_null);
    CHECK(allocator_realloc(NULL, 0) == NULL);
    CHECK(allocator_validate());
    CHECK(allocator_destroy());
    return true;
}
static bool growth_and_exhaustion(void) {
    CHECK(start_heap(MIB, 4 * MIB, ALLOCATOR_FIRST_FIT));
    void *a = allocator_alloc(MIB * 2);
    CHECK(a);
    memset(a, 42, MIB * 2);
    AllocatorStats s;
    CHECK(allocator_get_stats(&s));
    CHECK(s.regions == 2 && s.reserved_bytes <= 4 * MIB);
    CHECK(allocator_alloc(8 * MIB) == NULL);
    allocator_free(a);
    CHECK(allocator_get_stats(&s));
    CHECK(s.free_blocks == s.regions);
    CHECK(start_heap(MIB, MIB, ALLOCATOR_FIRST_FIT));
    CHECK(allocator_get_stats(&s));
    a = allocator_alloc(s.largest_free_block);
    CHECK(a && allocator_alloc(1) == NULL);
    allocator_free(a);
    CHECK(allocator_validate());
    CHECK(allocator_destroy());
    return true;
}
static bool strategies(void) {
    for (int policy = ALLOCATOR_FIRST_FIT; policy <= ALLOCATOR_BEST_FIT; policy++) {
        CHECK(start_heap(MIB, MIB, (AllocatorStrategy)policy));
        void *small_block = allocator_alloc(512), *g1 = allocator_alloc(64);
        void *large = allocator_alloc(2048), *g2 = allocator_alloc(64);
        CHECK(small_block && g1 && large && g2);
        allocator_free(small_block);
        allocator_free(large); /* Most recently freed block is first in the list. */
        void *chosen = allocator_alloc(256);
        CHECK(chosen == (policy == ALLOCATOR_FIRST_FIT ? large : small_block));
        allocator_free(chosen);
        allocator_free(g1);
        allocator_free(g2);
        CHECK(allocator_validate());
        CHECK(allocator_destroy());
    }
    return true;
}
static uint32_t random_next(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}
static bool randomized_sequences(void) {
    enum { SLOTS = 64, OPERATIONS = 10000 };
    for (int policy = ALLOCATOR_FIRST_FIT; policy <= ALLOCATOR_BEST_FIT; policy++) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            CHECK(start_heap(MIB, 4 * MIB, (AllocatorStrategy)policy));
            unsigned char *p[SLOTS] = {0};
            size_t sizes[SLOTS] = {0};
            uint32_t random = seed;
            for (int op = 0; op < OPERATIONS; op++) {
                size_t i = random_next(&random) % SLOTS;
                unsigned char value = (unsigned char)(i + 1);
                if (p[i]) CHECK(matches(p[i], sizes[i], value));
                if (p[i] && random_next(&random) % 3 == 0) {
                    allocator_free(p[i]);
                    p[i] = NULL;
                    sizes[i] = 0;
                } else {
                    size_t size = 1 + random_next(&random) % 4096;
                    unsigned char *next = allocator_realloc(p[i], size);
                    CHECK(next);
                    if (p[i]) CHECK(matches(next, sizes[i] < size ? sizes[i] : size, value));
                    memset(next, value, size);
                    p[i] = next;
                    sizes[i] = size;
                }
                if (op % 100 == 0) CHECK(allocator_validate());
            }
            for (size_t i = 0; i < SLOTS; i++) {
                if (p[i]) CHECK(matches(p[i], sizes[i], (unsigned char)(i + 1)));
                allocator_free(p[i]);
            }
            CHECK(allocator_validate());
            CHECK(allocator_destroy());
        }
    }
    return true;
}

typedef struct { unsigned char pattern; bool success; } Worker;
static bool worker_body(Worker *worker) {
    for (size_t iteration = 0; iteration < 4000; iteration++) {
        size_t size = 1 + iteration % 1024;
        unsigned char *p = allocator_calloc(size, 1);
        if (!p || !matches(p, size, 0)) return false;
        memset(p, worker->pattern, size);
        unsigned char *next = allocator_realloc(p, size + 127);
        if (!next || !matches(next, size, worker->pattern)) return false;
        allocator_free(next);
        if (iteration % 400 == 0 && !allocator_validate()) return false;
    }
    return true;
}
#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID data) {
    Worker *worker = data;
    worker->success = worker_body(worker);
    return 0;
}
#else
static void *worker_entry(void *data) {
    Worker *worker = data;
    worker->success = worker_body(worker);
    return NULL;
}
#endif
static bool concurrent_use(void) {
    CHECK(allocator_destroy()); /* Exercise concurrent lazy heap initialization. */
    Worker workers[4] = {{1, false}, {2, false}, {3, false}, {4, false}};
#ifdef _WIN32
    HANDLE threads[4];
    for (int i = 0; i < 4; i++) {
        threads[i] = CreateThread(NULL, 0, worker_entry, &workers[i], 0, NULL);
        CHECK(threads[i] != NULL);
    }
    CHECK(WaitForMultipleObjects(4, threads, TRUE, INFINITE) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; i++) CloseHandle(threads[i]);
#else
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) CHECK(pthread_create(&threads[i], NULL, worker_entry, &workers[i]) == 0);
    for (int i = 0; i < 4; i++) CHECK(pthread_join(threads[i], NULL) == 0);
#endif
    for (int i = 0; i < 4; i++) CHECK(workers[i].success);
    AllocatorStats s;
    CHECK(allocator_get_stats(&s));
    CHECK(s.active_allocations == 0 && s.requested_bytes == 0);
    CHECK(s.invalid_operations == 0 && s.failed_allocations == 0);
    CHECK(allocator_destroy());
    return true;
}
int main(void) {
    const struct { const char *name; bool (*run)(void); } tests[] = {
        {"concurrent use and lazy initialization", concurrent_use},
        {"configuration and lifecycle", configuration},
        {"alignment and overflow", alignment_and_overflow},
        {"calloc reuses and clears memory", zero_initialization},
        {"coalescing and statistics", coalescing_and_statistics},
        {"invalid pointers and double frees", invalid_operations},
        {"realloc preservation and failures", resizing},
        {"OS growth and heap exhaustion", growth_and_exhaustion},
        {"first-fit and best-fit choices", strategies},
        {"60,000 randomized operations", randomized_sequences}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!tests[i].run()) return 1;
        printf("PASS %s\n", tests[i].name);
    }
    puts("All 10 test groups passed.");
    return 0;
}
