# Allocator memory profile

- Scale: `0.1` of the specified workload counts; per-container sizes (hash fields, list elements, set members, sorted-set members) were scaled too.
- Seed: `20261109`
- Process model: a fresh server process for every workload.
- Settling: RSS is re-sampled until it is stable for up to 12 s before each measurement.
- Clone jemalloc: `/usr/lib/x86_64-linux-gnu/libjemalloc.so.2` with `MALLOC_CONF=background_thread:true`. Redis uses its own bundled allocator and defaults, so versions and tuning can differ.
- Trim rows (`*-trim`): not collected (`TRIM=1` enables them).
- Full logical snapshots matched across glibc, jemalloc, and Redis for: W1-fill, W2-1024-fill, W2-256-fill, W3-fill, W4-fill, W5-fill, W6-fill, W7-round3-refill.
- `used_bytes` is the clone's logical payload estimate; allocator allocated/resident includes runtime and container overhead. glibc resident is the `mallinfo2` `arena + hblkhd` estimate, which is heap extent: it does not fall when pages are released with madvise, so judge what was returned to the OS by `process_rss_bytes`. jemalloc uses refreshed `stats.allocated` and `stats.resident`.
- `fragmentation_ratio` is allocator resident / allocated for the clone and Redis's `mem_fragmentation_ratio` for the reference, so it is not comparable across targets. `rss_over_allocated` is process RSS / allocator allocated computed the same way for all three; use it for cross-target comparison. `bytes_per_key` uses logical used bytes (Redis `used_memory`) / live keys.
- Clone accounting tolerance compares the change from the per-process baseline: `abs(allocator_allocated_bytes - accounting_baseline_bytes - used_bytes) <= max(1 MiB, 25% of used_bytes)` for populated phases; rows outside that bound are marked `investigate` in the CSV.
- Highest clone logical bytes/key among W3-W6 in the glibc run: W6-fill at 1575080.000 bytes/key. Compare the allocator columns in the CSV to quantify container overhead.
- W1 allocator-allocated bytes/key above the process baseline (a 16-byte key plus a 64-byte value is 80): glibc 344, jemalloc 360, redis 167.
- The clone's logical counter charges 160 bytes/key for a W1 entry, i.e. 80 beyond the 16-byte key and 64-byte value (`Object::logicalSize`).
- Accounting tolerance exceeded; investigate these measurements:
  - glibc, W1-fill: used_bytes=16000000, allocator_allocated_bytes=34477440, baseline=83296, delta=18394144
  - glibc, W2-256-fill: used_bytes=35200000, allocator_allocated_bytes=53677152, baseline=83296, delta=18393856
  - glibc, W3-fill: used_bytes=3350000, allocator_allocated_bytes=5345824, baseline=83296, delta=1912528
  - glibc, W4-fill: used_bytes=3440000, allocator_allocated_bytes=5187520, baseline=83296, delta=1664224
  - glibc, W5-fill: used_bytes=5280000, allocator_allocated_bytes=8706512, baseline=83296, delta=3343216
  - glibc, W7-fill: used_bytes=16000000, allocator_allocated_bytes=34477440, baseline=83296, delta=18394144
  - glibc, W7-round1-overwrite: used_bytes=25600000, allocator_allocated_bytes=44078608, baseline=83296, delta=18395312
  - glibc, W7-round1-delete: used_bytes=17919808, allocator_allocated_bytes=32565360, baseline=83296, delta=14562256
  - glibc, W7-round1-refill: used_bytes=22719808, allocator_allocated_bytes=41321056, baseline=83296, delta=18517952
  - glibc, W7-round2-overwrite: used_bytes=67353088, allocator_allocated_bytes=85955584, baseline=83296, delta=18519200
  - glibc, W7-round2-delete: used_bytes=47297152, allocator_allocated_bytes=62044736, baseline=83296, delta=14664288
  - glibc, W7-round2-refill: used_bytes=52097152, allocator_allocated_bytes=70821008, baseline=83296, delta=18640560
  - glibc, W7-round3-overwrite: used_bytes=37223808, allocator_allocated_bytes=55975696, baseline=83296, delta=18668592
  - glibc, W7-round3-delete: used_bytes=26103552, allocator_allocated_bytes=40944320, baseline=83296, delta=14757472
  - glibc, W7-round3-refill: used_bytes=30903552, allocator_allocated_bytes=49779792, baseline=83296, delta=18792944
  - glibc, W8-del-fill: used_bytes=3350000, allocator_allocated_bytes=5345824, baseline=83296, delta=1912528
  - glibc, W8-flush-fill: used_bytes=3350000, allocator_allocated_bytes=5347600, baseline=83296, delta=1914304
  - jemalloc, W1-fill: used_bytes=16000000, allocator_allocated_bytes=36322384, baseline=321888, delta=20000496
  - jemalloc, W2-256-fill: used_bytes=35200000, allocator_allocated_bytes=60324704, baseline=321888, delta=24802816
  - jemalloc, W2-1024-fill: used_bytes=112000000, allocator_allocated_bytes=156325568, baseline=321888, delta=44003680
  - jemalloc, W3-fill: used_bytes=3350000, allocator_allocated_bytes=5424976, baseline=321888, delta=1753088
  - jemalloc, W4-fill: used_bytes=3440000, allocator_allocated_bytes=5250944, baseline=321888, delta=1489056
  - jemalloc, W5-fill: used_bytes=5280000, allocator_allocated_bytes=8145376, baseline=321888, delta=2543488
  - jemalloc, W7-fill: used_bytes=16000000, allocator_allocated_bytes=36322384, baseline=321888, delta=20000496
  - jemalloc, W7-round1-overwrite: used_bytes=25600000, allocator_allocated_bytes=48324096, baseline=321888, delta=22402208
  - jemalloc, W7-round1-delete: used_bytes=17919808, allocator_allocated_bytes=35656800, baseline=321888, delta=17415104
  - jemalloc, W7-round1-refill: used_bytes=22719808, allocator_allocated_bytes=44711440, baseline=321888, delta=21669744
  - jemalloc, W7-round2-overwrite: used_bytes=67353088, allocator_allocated_bytes=100546256, baseline=321888, delta=32871280
  - jemalloc, W7-round2-delete: used_bytes=47297152, allocator_allocated_bytes=72388240, baseline=321888, delta=24769200
  - jemalloc, W7-round2-refill: used_bytes=52097152, allocator_allocated_bytes=81436400, baseline=321888, delta=29017360
  - jemalloc, W7-round3-overwrite: used_bytes=37223808, allocator_allocated_bytes=62918416, baseline=321888, delta=25372720
  - jemalloc, W7-round3-delete: used_bytes=26103552, allocator_allocated_bytes=45925264, baseline=321888, delta=19499824
  - jemalloc, W7-round3-refill: used_bytes=30903552, allocator_allocated_bytes=54929904, baseline=321888, delta=23704464
  - jemalloc, W8-del-fill: used_bytes=3350000, allocator_allocated_bytes=5424976, baseline=321888, delta=1753088
  - jemalloc, W8-flush-fill: used_bytes=3350000, allocator_allocated_bytes=5417440, baseline=321888, delta=1745552
- W1 and W2 hold only plain strings, yet glibc W1-fill is 184 bytes/key over the logical counter. So the counter is missing per-entry keyspace overhead and allocator size-class rounding, not only container costs. W3-W6 additionally omit hash/set bucket and node storage, deque blocks, and ZSet skip-list nodes plus the score index. The CSV records the observed delta per phase.
- Allocator bytes still allocated after the last free phase: glibc 0.7 MiB (W8-flushall-free), jemalloc 0.8 MiB (W8-flushall-free), redis 1.8 MiB (W8-flushall-free). A clone figure that does not depend on the data written points at a keyspace structure that FLUSHALL does not shrink.
- RSS handed back after freeing (`-free` rows compare with the fill before them; `-trim` rows with the free row):

  | target | phase | RSS before (MiB) | RSS after (MiB) | returned |
  |---|---|---:|---:|---:|
  | glibc | W1-free | 36.4 | 36.4 | 0% |
  | glibc | W2-256-free | 54.7 | 54.7 | 0% |
  | glibc | W2-1024-free | 127.9 | 128.0 | 0% |
  | glibc | W3-free | 9.3 | 9.3 | 0% |
  | glibc | W4-free | 9.2 | 9.2 | 0% |
  | glibc | W5-free | 12.6 | 12.6 | 0% |
  | glibc | W6-free | 8.5 | 8.6 | 0% |
  | glibc | W7-free | 96.4 | 96.4 | 0% |
  | glibc | W8-del-free | 9.3 | 9.3 | 0% |
  | glibc | W8-flushall-free | 9.3 | 9.3 | 0% |
  | jemalloc | W1-free | 39.7 | 39.7 | 0% |
  | jemalloc | W2-256-free | 62.9 | 62.9 | 0% |
  | jemalloc | W2-1024-free | 176.0 | 12.9 | 93% |
  | jemalloc | W3-free | 10.7 | 10.7 | 0% |
  | jemalloc | W4-free | 10.6 | 10.6 | 0% |
  | jemalloc | W5-free | 13.4 | 13.4 | 0% |
  | jemalloc | W6-free | 9.7 | 9.8 | 0% |
  | jemalloc | W7-free | 124.4 | 106.4 | 14% |
  | jemalloc | W8-del-free | 10.7 | 10.8 | 0% |
  | jemalloc | W8-flushall-free | 12.1 | 12.1 | 0% |
  | redis | W1-free | 31.3 | 15.3 | 51% |
  | redis | W2-256-free | 54.5 | 15.6 | 71% |
  | redis | W2-1024-free | 147.1 | 16.4 | 89% |
  | redis | W3-free | 16.2 | 15.0 | 7% |
  | redis | W4-free | 17.6 | 15.1 | 15% |
  | redis | W5-free | 16.8 | 15.1 | 10% |
  | redis | W6-free | 16.2 | 15.2 | 6% |
  | redis | W7-free | 115.8 | 15.9 | 86% |
  | redis | W8-del-free | 16.2 | 16.3 | 0% |
  | redis | W8-flushall-free | 16.3 | 15.1 | 8% |

- Raw phase measurements: [allocator_memory.csv](./allocator_memory.csv)
