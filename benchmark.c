#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "allocator.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

enum { SLOTS = 256, OPERATIONS = 100000, TRIALS = 5 };
static double seconds_now(void) {
#ifdef _WIN32
    LARGE_INTEGER value, frequency;
    QueryPerformanceCounter(&value);
    QueryPerformanceFrequency(&frequency);
    return (double)value.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
#endif
}
static uint32_t random_next(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}
static bool run_trial(int allocator, int workload, int trial) {
    void *(*allocate)(size_t) = allocator == 2 ? malloc : allocator_alloc;
    void (*release)(void *) = allocator == 2 ? free : allocator_free;
    if (allocator != 2) {
        AllocatorConfig config = {
            (size_t)1024 * 1024, (size_t)64 * 1024 * 1024,
            allocator == 0 ? ALLOCATOR_FIRST_FIT : ALLOCATOR_BEST_FIT
        };
        if (!allocator_configure(&config)) return false;
    }
    void *slots[SLOTS] = {0};
    uint32_t random = UINT32_C(20261007);
    size_t failures = 0;
    double begin = seconds_now();
    for (int op = 0; op < OPERATIONS; op++) {
        size_t index = random_next(&random) % SLOTS;
        /* Keep the random index and size stream identical across allocators. */
        uint32_t value = random_next(&random);
        size_t size = workload == 0 ? 16 + value % 8177 :
            (value % 5 ? 16 + value % 241 : 8192 + value % 57345);
        if (slots[index]) {
            release(slots[index]);
            slots[index] = NULL;
        } else {
            slots[index] = allocate(size);
            if (slots[index]) memset(slots[index], 0x5a, size);
            else failures++;
        }
    }
    double elapsed = seconds_now() - begin;
    AllocatorStats stats = {0};
    if (allocator != 2 && !allocator_get_stats(&stats)) return false;
    /* The snapshot is before cleanup; stats and cleanup are outside timing. */
    for (size_t i = 0; i < SLOTS; i++) release(slots[i]);
    if (allocator != 2 && (!allocator_validate() || !allocator_destroy())) return false;
    static const char *names[] = {"first_fit", "best_fit", "system_malloc"};
    printf("%s,%s,%d,%d,%.6f,%.0f,%zu,", names[allocator],
           workload == 0 ? "mixed" : "bimodal", trial, OPERATIONS,
           elapsed, elapsed > 0 ? OPERATIONS / elapsed : 0, failures);
    if (allocator == 2) puts("NA,NA,NA,NA");
    else {
        double fragmentation = stats.free_bytes ?
            100.0 * (1.0 - (double)stats.largest_free_block / stats.free_bytes) : 0;
        printf("%zu,%zu,%zu,%.2f\n", stats.reserved_bytes, stats.free_bytes,
               stats.largest_free_block, fragmentation);
    }
    return failures == 0;
}
int main(void) {
    puts("allocator,workload,trial,operations,seconds,ops_per_second,failures,reserved_bytes,free_bytes,largest_free_block,external_fragmentation_pct");
    for (int workload = 0; workload < 2; workload++) {
        for (int trial = 1; trial <= TRIALS; trial++) {
            /* Rotate order to reduce consistent first-run bias. */
            for (int offset = 0; offset < 3; offset++) {
                int allocator = (trial - 1 + offset) % 3;
                if (!run_trial(allocator, workload, trial)) {
                    fputs("Benchmark failed: allocation, validation, or cleanup error.\n", stderr);
                    return 1;
                }
            }
        }
    }
    return 0;
}
