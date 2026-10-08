# Memory Allocator in C

A custom allocator with OS-backed growth, selectable first-fit/best-fit allocation,
block splitting and coalescing, calloc/realloc, heap diagnostics, and thread safety.

Built and checked on Windows using the installed MinGW GCC 6.3.0 compiler (32-bit
target). A Linux implementation using mmap and pthreads is included but has not
been executed in this environment.

## Run on your Windows computer

Open PowerShell in this folder:

```powershell
cd "C:\Users\Admin\Documents\Codex\2026-09-28\can-x20\outputs\memory-allocator"
.\build.ps1 -Test -Benchmark
.\allocator_demo.exe
.\allocator_demo.exe --best-fit
```

The script builds the demo, tests, and benchmark with warnings treated as errors.
-Test runs the correctness checks. -Benchmark saves measurements to
benchmark-results.csv. Both switches are optional.

Manual demo build:

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -D__USE_MINGW_ANSI_STDIO=1 allocator.c main.c -o allocator_demo.exe
.\allocator_demo.exe
```

The MinGW flag enables portable size_t formatting in printf with this compiler.

Linux build commands:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic allocator.c main.c -pthread -o allocator_demo
cc -std=c11 -O2 -Wall -Wextra -Wpedantic allocator.c tests.c -pthread -o allocator_tests
cc -std=c11 -O2 -Wall -Wextra -Wpedantic allocator.c benchmark.c -pthread -o allocator_bench
./allocator_tests
./allocator_demo
./allocator_bench > benchmark-results.csv
```

## What changed

| Feature | Behavior |
| --- | --- |
| Free-block list | Allocation searches only available blocks. Physical neighbor links support coalescing. |
| Two strategies | First-fit takes the first suitable free block; best-fit takes the smallest suitable block. |
| Growable heap | Windows VirtualAlloc / Linux mmap obtains more regions as needed, within a configured limit. |
| calloc | Checks multiplication overflow and clears every requested byte, including reused storage. |
| realloc | Shrinks or grows in place when possible; otherwise copies to a new block. Failure preserves the original allocation. |
| Statistics | Reports live and peak requested bytes, free capacity, largest free block, metadata, regions, and operation counts. |
| Heap checker | Checks alignment, block sizes and links, free-list membership, coalescing, and accounting. |
| Thread safety | Every public operation holds one process-wide mutex. Internal helpers avoid recursive locking. |
| Lifecycle | Initialization is idempotent. Destroy refuses while allocations remain live. |

## API example

```c
#include "allocator.h"
#include <string.h>

int example(void) {
    AllocatorConfig config = {
        1024u * 1024u,          /* Initial OS region: 1 MiB */
        16u * 1024u * 1024u,    /* Maximum total region bytes: 16 MiB */
        ALLOCATOR_BEST_FIT
    };
    if (!allocator_configure(&config)) return 1;

    char *text = allocator_calloc(32, 1);
    if (!text) {
        allocator_destroy();
        return 1;
    }
    strcpy(text, "hello");

    char *resized = allocator_realloc(text, 128);
    if (resized) text = resized; /* Keep text if resizing fails. */

    allocator_free(text);
    return allocator_destroy() ? 0 : 1;
}
```

You can also call allocator_alloc directly. Lazy initialization selects first-fit,
a 1 MiB initial region, and a 64 MiB total limit. allocator_init uses the same
defaults and does not reset an existing heap. Custom configuration must happen
before initialization. Destroy and reconfigure to change strategies.

## Allocation rules

- Allocations align to alignof(max_align_t); over-aligned types are not supported.
- alloc(0), calloc with a zero factor, and realloc(NULL, 0) return NULL.
- realloc(NULL, n) acts like alloc(n). realloc(p, 0) frees p and returns NULL.
- Failed nonzero realloc keeps p valid and its contents unchanged.
- free(NULL) does nothing. Foreign pointers, interior pointers, and recognizable
  double frees are ignored and counted as invalid_operations; invalid realloc
  returns NULL. An old address reused for another allocation cannot be recognized
  as stale.
- Never pass these pointers to the C library's free or realloc.
- A single large request must fit in one region. Separate OS regions never merge.
- The initial size rounds up to an OS page. The maximum is a hard limit on total
  mapped region bytes, including metadata, not just user data. Set initial_size
  and max_size to the same page-aligned size for a fixed heap.
- Free regions remain available for reuse until allocator_destroy returns them
  all to the OS. Individual frees do not reduce reserved_bytes.
- The mutex protects allocator metadata. The caller still coordinates accesses
  to shared payloads and ensures a pointer is no longer used after it is freed.

## How the allocator works

Each OS region contains a region header followed by blocks. Each block has a
header and aligned payload:

```text
Region
  [region header]
  [block header | allocated payload]
  [block header | free capacity]
  [block header | allocated payload]

Physical links: neighboring blocks within the same region
Free-list links: free blocks across all regions
```

When allocating, round the requested size to the alignment, select a free block,
remove it from the free list, and split off a usable remainder. If no block fits,
request another OS region, subject to the limit.

When freeing, locate the exact payload pointer and merge its free neighbors.
Insert the resulting block at the front of the free list. First-fit therefore
uses free-list order, not increasing memory-address order.

The allocation search is O(F) for F free blocks. Best-fit examines all candidates
unless it finds an exact match. Pointer validation in free and realloc is O(B)
for B total blocks. Actual splitting and neighbor merging are O(1).

The checker first validates each physical chain, then verifies free-list
membership. It is a diagnostic operation with potentially quadratic work.
Statistics invoke the checker and are intended for observation, not hot loops.
The checker assumes the OS-region list itself is intact; it cannot safely
recover from arbitrary memory corruption. Allocator metadata does not prevent
buffer overruns or use-after-free in calling code.

## Reading the statistics

- requested_bytes: bytes currently requested by callers.
- allocated_capacity: aligned capacity held by live blocks.
- peak_requested_bytes: highest simultaneous requested bytes, including temporary
  overlap between old and replacement blocks during a moving realloc.
- allocation_count/free_count: block acquisitions/releases, including realloc
  replacements. Resizing in place does not increment these counts.
- failed_allocations: nonzero requests rejected for overflow, capacity limits,
  or OS allocation failure. Zero-size requests do not count as failures.
- metadata_bytes: region and block headers, including their alignment padding.

The accounting identity is:

```text
reserved_bytes = allocated_capacity + free_bytes + metadata_bytes
```

The demo and benchmark use this external fragmentation indicator:

```text
100 * (1 - largest_free_block / free_bytes)
```

It is zero when there are no free bytes. Higher values mean free capacity is
spread over smaller blocks. It includes separation between OS regions, so it can
remain above zero after every allocation has been freed. Internal fragmentation
is allocated_capacity minus requested_bytes.

## Checks and benchmarks

The test executable covers 10 groups: lifecycle/configuration, alignment and
overflow, calloc reuse, splitting/coalescing/accounting, invalid pointers,
realloc behavior, heap growth/exhaustion, strategy selection, randomized
sequences, and concurrent use. It performs 60,000 randomized operations across
both policies and three seeds, plus 16,000 worker iterations across four threads.
Worker iterations include calloc, realloc, data checks, and free.

The benchmark compares both strategies with the system malloc/free using two
repeatable workloads, 100,000 operations per trial and five trials. Read
BENCHMARK.md for the methodology, recorded results, and limitations.

## Files

- allocator.h / allocator.c: public API and implementation.
- main.c: demonstration of allocation, resizing, growth, and statistics.
- tests.c: correctness and concurrency checks.
- benchmark.c: repeatable allocation workloads.
- build.ps1: Windows build and optional checks.
- benchmark-results.csv / BENCHMARK.md: recorded measurements and explanation.

## Current scope and future work

This is an educational allocator, not a drop-in production replacement for
malloc. A global mutex serializes allocator operations. Further experiments
could add size-segregated bins, thread-local arenas, optional guard pages,
returning empty regions early, or indexed pointer lookup to speed up frees.

