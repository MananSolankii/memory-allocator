# Benchmark report

Recorded on 7 October 2026 using Windows, MinGW.org GCC 6.3.0, mingw32 target,
and an AMD64 host (Family 23, Model 96). Compiler flags:
-std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -D__USE_MINGW_ANSI_STDIO=1.

## Recorded results

The throughput values below are medians of five trials, in millions of operations
per second. Fragmentation and region size are snapshots before final cleanup.
The raw measurements are in benchmark-results.csv.

| Workload | Allocator | Median Mops/s | Mapped MiB | External fragmentation |
| --- | --- | ---: | ---: | ---: |
| Mixed | First-fit | 2.794 | 1 | 88.23% |
| Mixed | Best-fit | 2.326 | 1 | 28.82% |
| Mixed | System malloc | 1.858 | Not measured | Not measured |
| Bimodal | First-fit | 2.885 | 3 | 70.85% |
| Bimodal | Best-fit | 2.703 | 2 | 38.01% |
| Bimodal | System malloc | 2.321 | Not measured | Not measured |

All 30 trials completed with zero failed allocations and passed the applicable
custom-heap consistency and cleanup checks. In these traces, first-fit had higher
median throughput than best-fit, while best-fit left larger contiguous free blocks
and used one fewer MiB in the bimodal case.

## Reproduce

From this folder in PowerShell:

```powershell
.\build.ps1 -Test -Benchmark
```

This overwrites benchmark-results.csv with your new measurements. This report is
a snapshot of the original run, so its numbers do not update automatically.

## Workloads and timing

- One thread, 256 pointer slots, 100,000 timed operations per trial.
- A deterministic xorshift32 generator starts from seed 20261007 for every trial.
- An occupied slot is freed; an empty slot is allocated and filled using memset.
  Allocation requests and memory filling are included in timing.
- Mixed requests range from 16 through 8,192 bytes.
- Bimodal requests use roughly 80% small blocks (16-256 bytes) and 20% large
  blocks (8-64 KiB).
- Five trials per allocator per workload, with allocator order rotated.
- Custom heaps start at 1 MiB and may grow up to 64 MiB.
- Custom heap initialization, statistics, and final cleanup are outside timing.
  Additional region growth during the workload is inside timing.
- The Windows timer is QueryPerformanceCounter. The Linux branch uses
  clock_gettime(CLOCK_MONOTONIC).
- Fragmentation is 100 * (1 - largest_free_block / free_bytes), measured before
  final cleanup. It includes the effects of separate OS regions.

## Interpretation limits

These are short synthetic allocation-and-write workloads on one Windows machine
using its installed 32-bit runtime. The system allocator retains its own state
between trials, while each custom heap is recreated. Custom initial-region setup
is excluded; there is no separate warm-up phase. Background activity and page
faults can affect the timings, and the first bimodal system-malloc trial was
noticeably slower than later ones.

These measurements do not establish performance for other applications, operating
systems, or contemporary 64-bit allocators. They do not measure peak resident
memory, latency distributions, or multithreaded throughput. The custom allocator
retains mapped regions until destruction, and its free operation performs a
linear pointer lookup. These limitations should accompany any resume or
presentation claim based on the results.

