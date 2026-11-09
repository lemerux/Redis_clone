#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${BUILD_DIR:-/tmp/redis-clone-expiry-benchmark-build}

cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_EXPIRY_BENCHMARKS=ON >&2
cmake --build "$build_dir" --target expiry-benchmark --parallel >&2

binary="$build_dir/expiry-benchmark"
jemalloc_library=$(ldconfig -p |
    awk '/libjemalloc\.so(\.|$)/ { print $NF; exit }')
if [ -z "$jemalloc_library" ]; then
    echo "libjemalloc was not found by ldconfig" >&2
    exit 1
fi

printf '%s\n' \
    'allocator,key_count,budget_us,max_cycle_us,population_ms,expiry_ms,rss_before_bytes,rss_after_bytes,expired_keys,budget_status'
"$binary" --allocator glibc "$@"
env LD_PRELOAD="$jemalloc_library" "$binary" --allocator jemalloc "$@"
