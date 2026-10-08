#include "allocator.h"
#include <stdio.h>
#include <string.h>

static void print_stats(const char *label) {
    AllocatorStats s;
    if (!allocator_get_stats(&s)) {
        puts("Heap consistency check FAILED");
        return;
    }
    double fragmentation = s.free_bytes ?
        100.0 * (1.0 - (double)s.largest_free_block / s.free_bytes) : 0;
    printf("\n%s\n", label);
    printf("  Live allocations: %zu | Requested bytes: %zu | Peak: %zu\n",
           s.active_allocations, s.requested_bytes, s.peak_requested_bytes);
    printf("  Free bytes: %zu | Largest free block: %zu | Fragmentation: %.2f%%\n",
           s.free_bytes, s.largest_free_block, fragmentation);
    printf("  OS regions: %zu | Reserved bytes: %zu | Metadata bytes: %zu\n",
           s.regions, s.reserved_bytes, s.metadata_bytes);
}
int main(int argc, char **argv) {
    bool best_fit = argc == 2 && strcmp(argv[1], "--best-fit") == 0;
    if (argc > 2 || (argc == 2 && !best_fit)) {
        fprintf(stderr, "Usage: %s [--best-fit]\n", argv[0]);
        return 1;
    }
    AllocatorConfig config = {
        (size_t)1024 * 1024, (size_t)16 * 1024 * 1024,
        best_fit ? ALLOCATOR_BEST_FIT : ALLOCATOR_FIRST_FIT
    };
    if (!allocator_configure(&config)) {
        fputs("Could not initialize allocator.\n", stderr);
        return 1;
    }
    printf("Memory allocator demo (%s)\n", best_fit ? "best fit" : "first fit");
    print_stats("Initial heap");
    char *message = allocator_alloc(32);
    int *numbers = allocator_calloc(4, sizeof(*numbers));
    if (!message || !numbers) {
        allocator_free(message);
        allocator_free(numbers);
        allocator_destroy();
        return 1;
    }
    strcpy(message, "Hello from my allocator!");
    printf("\nStored: %s\n", message);
    printf("calloc numbers: %d %d %d %d\n", numbers[0], numbers[1], numbers[2], numbers[3]);
    char *larger = allocator_realloc(message, 128);
    if (!larger) {
        allocator_free(message);
        allocator_free(numbers);
        allocator_destroy();
        return 1;
    }
    message = larger;
    strcat(message, " The contents survived realloc.");
    puts(message);
    print_stats("After allocation and resizing");
    void *large = allocator_alloc((size_t)2 * 1024 * 1024);
    if (!large) {
        allocator_free(message);
        allocator_free(numbers);
        allocator_destroy();
        return 1;
    }
    print_stats("After requesting 2 MiB: the heap grows");
    allocator_free(numbers);
    allocator_free(message);
    allocator_free(large);
    print_stats("After freeing everything: one free block per region");
    printf("\nHeap check: %s\n", allocator_validate() ? "PASS" : "FAIL");
    if (!allocator_destroy()) return 1;
    puts("All regions returned to the operating system.");
    return 0;
}

