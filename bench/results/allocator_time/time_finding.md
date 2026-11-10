# Allocator timing profile

- Scale `0.5`, seed `20261109`, 3 repeat(s); every number is the median over fresh server processes (a new process per group and repeat).
- Clone jemalloc: `/usr/lib/x86_64-linux-gnu/libjemalloc.so.2` with `MALLOC_CONF=background_thread:true`. Redis uses its own bundled allocator.
- CPU pinning: none. Set `SERVER_CPU` and `CLIENT_CPU` for steadier numbers.
- Phase durations are timed around the Python pipelined client, so the client is the bottleneck. Compare targets with each other; do not read them as server limits. `FLUSHALL` is timed until its reply only.
- Latency mode is 1 client, no pipelining. Throughput mode is 1 client(s) with pipeline 16; its latency figures are per pipelined batch and are not comparable to latency mode. `spread` is (max - min) / median across repeats; treat differences smaller than it as noise.
- Redis is the reference target and is a different program. Compare glibc with jemalloc to isolate the allocator, and the clone with Redis to see the gap.

## Phase duration (seconds, median)

| phase | ops | glibc | jemalloc | redis | jemalloc/glibc | redis/glibc |
|---|---:|---:|---:|---:|---:|---:|
| W1-fill | 500000 | 4.031 | 4.300 | 3.465 | 1.07x | 0.86x |
| W1-free | 1 | 0.196 | 0.107 | 0.227 | 0.55x | 1.16x |
| W2-256-fill | 500000 | 4.668 | 4.702 | 3.552 | 1.01x | 0.76x |
| W2-256-free | 1 | 0.221 | 0.118 | 0.237 | 0.53x | 1.07x |
| W2-1024-fill | 500000 | 5.804 | 5.882 | 4.135 | 1.01x | 0.71x |
| W2-1024-free | 1 | 0.208 | 0.116 | 0.306 | 0.56x | 1.47x |
| W3-fill | 50000 | 1.081 | 1.060 | 0.915 | 0.98x | 0.85x |
| W3-free | 1 | 0.039 | 0.023 | 0.011 | 0.60x | 0.28x |
| W4-fill | 50000 | 2.947 | 2.916 | 2.768 | 0.99x | 0.94x |
| W4-free | 1 | 0.017 | 0.008 | 0.023 | 0.44x | 1.32x |
| W5-fill | 50000 | 1.953 | 1.902 | 1.804 | 0.97x | 0.92x |
| W5-free | 1 | 0.133 | 0.078 | 0.021 | 0.58x | 0.16x |
| W6-fill | 1250 | 0.617 | 0.574 | 0.460 | 0.93x | 0.75x |
| W6-free | 1 | 0.047 | 0.031 | 0.043 | 0.67x | 0.91x |
| W7-fill | 500000 | 4.278 | 5.002 | 3.467 | 1.17x | 0.81x |
| W7-round1-overwrite | 250000 | 1.837 | 2.758 | 1.889 | 1.50x | 1.03x |
| W7-round1-delete | 150000 | 1.033 | 1.155 | 0.942 | 1.12x | 0.91x |
| W7-round1-refill | 150000 | 1.477 | 1.041 | 1.032 | 0.70x | 0.70x |
| W7-round2-overwrite | 250000 | 2.987 | 2.762 | 2.063 | 0.92x | 0.69x |
| W7-round2-delete | 150000 | 1.347 | 1.097 | 0.964 | 0.81x | 0.72x |
| W7-round2-refill | 150000 | 1.291 | 1.153 | 1.058 | 0.89x | 0.82x |
| W7-round3-overwrite | 250000 | 1.974 | 2.260 | 1.921 | 1.15x | 0.97x |
| W7-round3-delete | 150000 | 1.051 | 1.206 | 0.962 | 1.15x | 0.92x |
| W7-round3-refill | 150000 | 1.095 | 1.129 | 1.048 | 1.03x | 0.96x |
| W7-free | 1 | 0.326 | 0.141 | 0.285 | 0.43x | 0.87x |
| W8-del-fill | 50000 | 1.076 | 1.071 | 0.900 | 1.00x | 0.84x |
| W8-del-free | 50000 | 0.094 | 0.072 | 0.050 | 0.76x | 0.53x |
| W8-flush-fill | 50000 | 1.024 | 1.085 | 0.926 | 1.06x | 0.90x |
| W8-flushall-free | 1 | 0.029 | 0.014 | 0.010 | 0.47x | 0.34x |

## Per-command latency, 1 client no pipelining

| test | rps glibc | rps jemalloc | rps redis | p50 ms glibc | p50 ms jemalloc | p50 ms redis | p99 ms glibc | p99 ms jemalloc | p99 ms redis |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SET-64 | 34602  | 34722  | 39683 | 0.031  | 0.031  | 0.023 | 0.047  | 0.055  | 0.047 |
| GET-64 | 34722  | 34130  | 42918 | 0.023  | 0.031  | 0.023 | 0.055  | 0.047  | 0.039 |
| SET-256 | 34364  | 31546  | 37175 | 0.023  | 0.031  | 0.023 | 0.055  | 0.071  | 0.047 |
| GET-256 | 33670  | 33557  | 42017 | 0.023  | 0.031  | 0.023 | 0.055  | 0.055  | 0.039 |
| SET-1024 | 31949  | 34014  | 34965 | 0.031  | 0.031  | 0.031 | 0.071  | 0.047  | 0.047 |
| GET-1024 | 33445  | 34247  | 40486 | 0.031  | 0.031  | 0.023 | 0.055  | 0.047  | 0.047 |
| LPUSH | 34483  | 35461  | 37594 | 0.031  | 0.031  | 0.023 | 0.055  | 0.047  | 0.047 |
| RPUSH | 35714  | 35088  | 41667 | 0.023  | 0.031  | 0.023 | 0.047  | 0.055  | 0.047 |
| LPUSH-many | 32895  | 34722  | 40486 | 0.031  | 0.031  | 0.023 | 0.063  | 0.055  | 0.047 |
| RPUSH-many | 32468  | 34130  | 41667 | 0.031  | 0.031  | 0.023 | 0.055  | 0.055  | 0.047 |
| LPOP | 34014  | 34483  | 41494 | 0.031  | 0.031  | 0.023 | 0.055  | 0.055  | 0.047 |
| RPOP | 34602  | 35336  | 39216 | 0.031  | 0.031  | 0.023 | 0.055  | 0.047  | 0.047 |
| LINDEX-head | 34722  | 33003  | 41494 | 0.031  | 0.031  | 0.023 | 0.055  | 0.055  | 0.047 |
| LINDEX-mid | 22624  | 23364  | 37453 | 0.039  | 0.039  | 0.023 | 0.071  | 0.071  | 0.047 |
| LINDEX-tail | 33784  | 34483  | 36364 | 0.031  | 0.031  | 0.023 | 0.055  | 0.055  | 0.055 |
| LSET-head-samelen | 35971  | 35211  | 42017 | 0.023  | 0.031  | 0.023 | 0.047  | 0.047  | 0.039 |
| LSET-mid-samelen | 18832  | 18904  | 39683 | 0.047  | 0.047  | 0.023 | 0.079  | 0.087  | 0.047 |
| LSET-tail-samelen | 34602  | 33003  | 38168 | 0.031  | 0.031  | 0.023 | 0.047  | 0.055  | 0.047 |

## Throughput, 1 client(s) pipeline 16

| test | rps glibc | rps jemalloc | rps redis | p50 ms glibc | p50 ms jemalloc | p50 ms redis | p99 ms glibc | p99 ms jemalloc | p99 ms redis |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SET-64 | 218818  | 209644  | 383142 | 0.063  | 0.071  | 0.039 | 0.119  | 0.127  | 0.071 |
| GET-64 | 236407  | 234742  | 452489 | 0.063  | 0.063  | 0.031 | 0.143  | 0.095  | 0.063 |
| SET-256 | 210526  | 210084  | 392157 | 0.071  | 0.071  | 0.039 | 0.119  | 0.159  | 0.071 |
| GET-256 | 224719  | 210526  | 420168 | 0.063  | 0.071  | 0.031 | 0.111  | 0.119  | 0.063 |
| SET-1024 | 177305  | 171821  | 313480 | 0.079  | 0.087  | 0.047 | 0.207  | 0.207  | 0.095 |
| GET-1024 | 194175  | 184162  | 338983 | 0.071  | 0.071  | 0.039 | 0.159  | 0.151  | 0.071 |
| LPUSH | 218818  | 193050  | 416667 | 0.063  | 0.071  | 0.031 | 0.119  | 0.175  | 0.063 |
| RPUSH | 219780  | 199203  | 434783 | 0.063  | 0.071  | 0.031 | 0.103  | 0.143  | 0.063 |
| LPUSH-many | 210526  | 192678  | 387597 | 0.071  | 0.079  | 0.039 | 0.159  | 0.143  | 0.079 |
| RPUSH-many | 199601  | 180832  | 389105 | 0.071  | 0.079  | 0.039 | 0.175  | 0.199  | 0.087 |
| LPOP | 241546  | 213675  | 380228 | 0.063  | 0.071  | 0.039 | 0.095  | 0.159  | 0.071 |
| RPOP | 236967  | 219780  | 414938 | 0.063  | 0.071  | 0.031 | 0.127  | 0.111  | 0.079 |
| LINDEX-head | 207900  | 190476  | 454545 | 0.071  | 0.079  | 0.031 | 0.143  | 0.143  | 0.063 |
| LINDEX-mid | 30230  | 23164  | 308642 | 0.535  | 0.679  | 0.047 | 0.951  | 0.951  | 0.095 |
| LINDEX-tail | 201613  | 192308  | 460830 | 0.071  | 0.079  | 0.031 | 0.159  | 0.119  | 0.055 |
| LSET-head-samelen | 223714  | 214592  | 436681 | 0.063  | 0.071  | 0.031 | 0.127  | 0.119  | 0.063 |
| LSET-mid-samelen | 28098  | 34118  | 307692 | 0.519  | 0.439  | 0.047 | 0.895  | 0.791  | 0.079 |
| LSET-tail-samelen | 229885  | 217391  | 440529 | 0.063  | 0.071  | 0.031 | 0.095  | 0.135  | 0.063 |

- Raw data: [time_phases.csv](./time_phases.csv), [time_commands.csv](./time_commands.csv)
