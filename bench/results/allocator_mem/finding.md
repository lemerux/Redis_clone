# Allocator memory profile

- Scale: `0.5` of the specified workload counts; per-container sizes (hash fields, list elements, set members, sorted-set members) were scaled too.
- Seed: `20261109`
- Process model: a fresh server process for every workload.
- Settling: RSS is re-sampled until it is stable for up to 12 s before each measurement.
- Clone jemalloc: `/usr/lib/x86_64-linux-gnu/libjemalloc.so.2` with `MALLOC_CONF=background_thread:true`. Redis uses its own bundled allocator and defaults, so versions and tuning can differ.
- Trim rows (`*-trim`): not collected (`TRIM=1` enables them).
- Full logical snapshots matched across glibc, jemalloc, and Redis for: W1-fill, W2-1024-fill, W2-256-fill, W3-fill, W4-fill, W5-fill, W6-fill, W7-round3-refill.
- `used_bytes` is the clone's logical payload estimate; allocator allocated/resident includes runtime and container overhead. glibc resident is the `mallinfo2` `arena + hblkhd` estimate, which is heap extent: it does not fall when pages are released with madvise, so judge what was returned to the OS by `process_rss_bytes`. jemalloc uses refreshed `stats.allocated` and `stats.resident`.
- `fragmentation_ratio` is allocator resident / allocated for the clone and Redis's `mem_fragmentation_ratio` for the reference, so it is not comparable across targets. `rss_over_allocated` is process RSS / allocator allocated computed the same way for all three; use it for cross-target comparison. `bytes_per_key` uses logical used bytes (Redis `used_memory`) / live keys.
- Clone accounting tolerance compares the change from the per-process baseline: `abs(allocator_allocated_bytes - accounting_baseline_bytes - used_bytes) <= max(1 MiB, 25% of used_bytes)` for populated phases; rows outside that bound are marked `investigate` in the CSV.
- Highest clone logical bytes/key among W3-W6 in the glibc run: W6-fill at 7892500.800 bytes/key. Compare the allocator columns in the CSV to quantify container overhead.
- W1 allocator-allocated bytes/key above the process baseline (a 16-byte key plus a 64-byte value is 80): glibc 333, jemalloc 350, redis 168.
- The clone's logical counter charges 160 bytes/key for a W1 entry, i.e. 80 beyond the 16-byte key and 64-byte value (`Object::logicalSize`).
- Accounting tolerance exceeded; investigate these measurements:
  - glibc, W1-fill: used_bytes=80000000, allocator_allocated_bytes=166578464, baseline=83296, delta=86495168
  - glibc, W2-256-fill: used_bytes=176000000, allocator_allocated_bytes=262578240, baseline=83296, delta=86494944
  - glibc, W3-fill: used_bytes=42150000, allocator_allocated_bytes=54879488, baseline=83296, delta=12646192
  - glibc, W4-fill: used_bytes=147200000, allocator_allocated_bytes=196881888, baseline=83296, delta=49598592
  - glibc, W5-fill: used_bytes=96800000, allocator_allocated_bytes=145279888, baseline=83296, delta=48396592
  - glibc, W6-fill: used_bytes=39462504, allocator_allocated_bytes=51588912, baseline=83296, delta=12043112
  - glibc, W7-fill: used_bytes=80000000, allocator_allocated_bytes=166578464, baseline=83296, delta=86495168
  - glibc, W7-round1-overwrite: used_bytes=128000000, allocator_allocated_bytes=214579472, baseline=83296, delta=86496176
  - glibc, W7-round1-delete: used_bytes=89574656, allocator_allocated_bytes=156961008, baseline=83296, delta=67303056
  - glibc, W7-round1-refill: used_bytes=113574656, allocator_allocated_bytes=200744064, baseline=83296, delta=87086112
  - glibc, W7-round2-overwrite: used_bytes=336731648, allocator_allocated_bytes=423902768, baseline=83296, delta=87087824
  - glibc, W7-round2-delete: used_bytes=235592000, allocator_allocated_bytes=303406048, baseline=83296, delta=67730752
  - glibc, W7-round2-refill: used_bytes=259592000, allocator_allocated_bytes=347303328, baseline=83296, delta=87628032
  - glibc, W7-round3-overwrite: used_bytes=185823168, allocator_allocated_bytes=273666224, baseline=83296, delta=87759760
  - glibc, W7-round3-delete: used_bytes=130160320, allocator_allocated_bytes=198436352, baseline=83296, delta=68192736
  - glibc, W7-round3-refill: used_bytes=154160320, allocator_allocated_bytes=242604944, baseline=83296, delta=88361328
  - glibc, W8-del-fill: used_bytes=42150000, allocator_allocated_bytes=54879488, baseline=83296, delta=12646192
  - glibc, W8-flush-fill: used_bytes=42150000, allocator_allocated_bytes=54880112, baseline=83296, delta=12646816
  - jemalloc, W1-fill: used_bytes=80000000, allocator_allocated_bytes=175223888, baseline=321888, delta=94902000
  - jemalloc, W2-256-fill: used_bytes=176000000, allocator_allocated_bytes=295226208, baseline=321888, delta=118904320
  - jemalloc, W2-1024-fill: used_bytes=560000000, allocator_allocated_bytes=775238880, baseline=321888, delta=214916992
  - jemalloc, W4-fill: used_bytes=147200000, allocator_allocated_bytes=196637600, baseline=321888, delta=49115712
  - jemalloc, W5-fill: used_bytes=96800000, allocator_allocated_bytes=127037936, baseline=321888, delta=29916048
  - jemalloc, W7-fill: used_bytes=80000000, allocator_allocated_bytes=175223888, baseline=321888, delta=94902000
  - jemalloc, W7-round1-overwrite: used_bytes=128000000, allocator_allocated_bytes=235224160, baseline=321888, delta=106902272
  - jemalloc, W7-round1-delete: used_bytes=89574656, allocator_allocated_bytes=171636144, baseline=321888, delta=81739600
  - jemalloc, W7-round1-refill: used_bytes=113574656, allocator_allocated_bytes=217170416, baseline=321888, delta=103273872
  - jemalloc, W7-round2-overwrite: used_bytes=336731648, allocator_allocated_bytes=496181760, baseline=321888, delta=159128224
  - jemalloc, W7-round2-delete: used_bytes=235592000, allocator_allocated_bytes=354189408, baseline=321888, delta=118275520
  - jemalloc, W7-round2-refill: used_bytes=259592000, allocator_allocated_bytes=399697280, baseline=321888, delta=139783392
  - jemalloc, W7-round3-overwrite: used_bytes=185823168, allocator_allocated_bytes=307570080, baseline=321888, delta=121425024
  - jemalloc, W7-round3-delete: used_bytes=130160320, allocator_allocated_bytes=222401360, baseline=321888, delta=91919152
  - jemalloc, W7-round3-refill: used_bytes=154160320, allocator_allocated_bytes=267917168, baseline=321888, delta=113434960
- W1 and W2 hold only plain strings, yet glibc W1-fill is 173 bytes/key over the logical counter. So the counter is missing per-entry keyspace overhead and allocator size-class rounding, not only container costs. W3-W6 additionally omit hash/set bucket and node storage, deque blocks, and ZSet skip-list nodes plus the score index. The CSV records the observed delta per phase.
- Allocator bytes still allocated after the last free phase: glibc 2.8 MiB (W8-flushall-free), jemalloc 2.9 MiB (W8-flushall-free), redis 1.8 MiB (W8-flushall-free). A clone figure that does not depend on the data written points at a keyspace structure that FLUSHALL does not shrink.
- RSS handed back after freeing (`-free` rows compare with the fill before them; `-trim` rows with the free row):

  | target | phase | RSS before (MiB) | RSS after (MiB) | returned |
  |---|---|---:|---:|---:|
  | glibc | W1-free | 162.6 | 162.6 | 0% |
  | glibc | W2-256-free | 254.1 | 254.1 | 0% |
  | glibc | W2-1024-free | 620.3 | 620.4 | 0% |
  | glibc | W3-free | 56.3 | 56.3 | 0% |
  | glibc | W4-free | 191.7 | 191.8 | 0% |
  | glibc | W5-free | 142.5 | 142.5 | 0% |
  | glibc | W6-free | 64.0 | 64.0 | 0% |
  | glibc | W7-free | 462.4 | 433.5 | 6% |
  | glibc | W8-del-free | 56.3 | 56.3 | 0% |
  | glibc | W8-flushall-free | 56.3 | 56.3 | 0% |
  | jemalloc | W1-free | 176.6 | 125.4 | 29% |
  | jemalloc | W2-256-free | 292.0 | 183.6 | 37% |
  | jemalloc | W2-1024-free | 772.3 | 456.4 | 41% |
  | jemalloc | W3-free | 55.1 | 55.1 | 0% |
  | jemalloc | W4-free | 198.7 | 198.7 | 0% |
  | jemalloc | W5-free | 128.9 | 128.9 | 0% |
  | jemalloc | W6-free | 62.5 | 52.6 | 16% |
  | jemalloc | W7-free | 596.3 | 32.2 | 95% |
  | jemalloc | W8-del-free | 55.1 | 55.1 | 0% |
  | jemalloc | W8-flushall-free | 94.4 | 11.2 | 88% |
  | redis | W1-free | 98.6 | 16.5 | 83% |
  | redis | W2-256-free | 214.1 | 17.5 | 92% |
  | redis | W2-1024-free | 675.8 | 21.3 | 97% |
  | redis | W3-free | 30.0 | 15.3 | 49% |
  | redis | W4-free | 69.8 | 16.7 | 76% |
  | redis | W5-free | 44.8 | 16.0 | 64% |
  | redis | W6-free | 46.0 | 15.9 | 65% |
  | redis | W7-free | 519.4 | 20.2 | 96% |
  | redis | W8-del-free | 30.1 | 30.1 | 0% |
  | redis | W8-flushall-free | 39.2 | 15.3 | 61% |

- Raw phase measurements: [allocator_memory.csv](./allocator_memory.csv)
