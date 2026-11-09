#!/bin/sh
# Allocator memory profile: clone on glibc, clone on jemalloc, reference Redis.
#
# Environment:
#   SCALE=1            fraction of the specified workload counts, in (0, 1]
#   SEED=20261109      payload seed
#   PER_WORKLOAD=1     start a fresh server for every workload (default) so a
#                      peak in one workload cannot leak into the next; 0 runs
#                      all workloads in one process like the old script
#   FIXED_SHAPE=0      1 scales only container counts and keeps per-container
#                      sizes (fields, elements, members) at the specified values
#   TRIM=0             1 adds a "-trim" row after each free phase: malloc_trim(0)
#                      and jemalloc purge via gdb on the clone, MEMORY PURGE on
#                      Redis. Needs gdb and ptrace permission on the server.
#   SETTLE_MAX=12      max seconds to wait for RSS to stop moving before each
#                      measurement (0 disables)
#   JEMALLOC_CONF      MALLOC_CONF for the preloaded jemalloc; default
#                      background_thread:true, which matches Redis's default
#                      jemalloc-bg-thread yes. Set empty to use jemalloc defaults.
set -eu

# shellcheck disable=SC1007
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1007
repo_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
source_dir="$repo_dir/src"
build_dir=${BUILD_DIR:-/tmp/redis-clone-allocator-mem-build}
results_dir=${RESULTS_DIR:-"$repo_dir/bench/results/allocator_mem"}
scale=${SCALE:-1}
seed=${SEED:-20261109}
python=${PYTHON:-python3}
per_workload=${PER_WORKLOAD:-1}
fixed_shape=${FIXED_SHAPE:-0}
trim=${TRIM:-0}
settle_max=${SETTLE_MAX:-12}
jemalloc_conf=${JEMALLOC_CONF-background_thread:true}

for tool in cmake "$python" redis-server ldconfig awk; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "required tool not found: $tool" >&2
        exit 1
    }
done

if [ "$trim" = 1 ] && ! command -v gdb >/dev/null 2>&1; then
    echo "TRIM=1 needs gdb for the clone targets; trim rows disabled for" \
        "all targets so the comparison stays symmetric" >&2
    trim=0
fi

jemalloc_library=$(ldconfig -p |
    awk '/libjemalloc\.so(\.|$)/ { print $NF; exit }')
if [ -z "$jemalloc_library" ]; then
    echo "libjemalloc was not found by ldconfig" >&2
    exit 1
fi
echo "clone jemalloc: $jemalloc_library (MALLOC_CONF=${jemalloc_conf:-unset})" >&2

if [ "$per_workload" = 1 ]; then
    groups="W1 W2 W3 W4 W5 W6 W7 W8"
else
    groups="ALL"
fi

extra_args="--scale $scale --seed $seed --settle-max $settle_max"
[ "$fixed_shape" = 1 ] && extra_args="$extra_args --fixed-shape"
[ "$trim" = 1 ] && extra_args="$extra_args --trim"

cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target redis-server --parallel
server="$build_dir/redis-server"
mkdir -p "$results_dir"
temporary_dir=$(mktemp -d)
server_pid=
redis_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if [ -n "$redis_pid" ]; then
        kill "$redis_pid" 2>/dev/null || true
        wait "$redis_pid" 2>/dev/null || true
    fi
    rm -rf "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

users_file="$temporary_dir/users.conf"
printf '%s\n' 'bench benchmark-password' > "$users_file"

available_port() {
    "$python" -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

# One fresh server process per group, so heap state never carries over
# between workloads (unless PER_WORKLOAD=0).
run_clone() {
    allocator=$1
    for group in $groups; do
        port=$(available_port)
        if [ "$allocator" = jemalloc ]; then
            env LD_PRELOAD="$jemalloc_library" MALLOC_CONF="$jemalloc_conf" \
                "$server" "$port" "$users_file" >/dev/null 2>&1 &
        else
            env -u LD_PRELOAD -u MALLOC_CONF \
                "$server" "$port" "$users_file" >/dev/null 2>&1 &
        fi
        server_pid=$!
        # $extra_args is intentionally unquoted: it holds separate flags.
        # shellcheck disable=SC2086
        "$python" "$script_dir/mem_profile.py" \
            --kind clone --allocator "$allocator" --port "$port" \
            --pid "$server_pid" --workloads "$group" $extra_args \
            --output "$temporary_dir/$allocator.$group.csv"
        kill "$server_pid"
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    done
}

run_reference() {
    for group in $groups; do
        port=$(available_port)
        redis-server --bind 127.0.0.1 --port "$port" --protected-mode no \
            --save "" --appendonly no --loglevel warning --daemonize no \
            --dir "$temporary_dir" >/dev/null 2>&1 &
        redis_pid=$!
        # shellcheck disable=SC2086
        "$python" "$script_dir/mem_profile.py" \
            --kind redis --allocator redis --port "$port" \
            --pid "$redis_pid" --workloads "$group" $extra_args \
            --output "$temporary_dir/redis.$group.csv"
        kill "$redis_pid"
        wait "$redis_pid" 2>/dev/null || true
        redis_pid=
    done
}

run_clone glibc
run_clone jemalloc
run_reference

"$python" - "$temporary_dir" "$results_dir" "$scale" "$seed" "$groups" \
    "$fixed_shape" "$trim" "$settle_max" "$jemalloc_conf" \
    "$jemalloc_library" <<'PY'
import csv
import os
import sys

(temporary_dir, results_dir, scale, seed, groups, fixed_shape, trim,
 settle_max, jemalloc_conf, jemalloc_library) = sys.argv[1:]
targets = ("glibc", "jemalloc", "redis")
group_list = groups.split()
MIB = 1024 * 1024

# Merge the per-group CSVs of each target. Every fresh process emits its own
# BASELINE row; keep only the first one.
rows_by_target = {}
for target in targets:
    merged = []
    for position, group in enumerate(group_list):
        path = os.path.join(temporary_dir, f"{target}.{group}.csv")
        with open(path, newline="", encoding="utf-8") as source:
            for row in csv.DictReader(source):
                if row["phase"] == "BASELINE" and position > 0:
                    continue
                merged.append(row)
    rows_by_target[target] = merged

digests = {}
for target, rows in rows_by_target.items():
    digests[target] = {
        row["phase"]: row["snapshot_sha256"]
        for row in rows
        if row["snapshot_sha256"]
    }
phases = set.intersection(*(set(value) for value in digests.values()))
if any(set(value) != phases for value in digests.values()):
    raise SystemExit("logical snapshot phase sets differ between targets")
for phase in sorted(phases):
    values = {digests[target][phase] for target in targets}
    if len(values) != 1:
        raise SystemExit("logical snapshot mismatch in " + phase)
if not phases:
    raise SystemExit("no logical snapshots were generated")

combined_path = os.path.join(results_dir, "allocator_memory.csv")
fields = list(rows_by_target["glibc"][0])
with open(combined_path, "w", newline="", encoding="utf-8") as output:
    writer = csv.DictWriter(output, fieldnames=fields)
    writer.writeheader()
    for target in targets:
        writer.writerows(rows_by_target[target])


def first_row(rows, phase):
    return next((row for row in rows if row["phase"] == phase), None)


def mib(value):
    return f"{int(value) / MIB:.1f}"


clone_rows = rows_by_target["glibc"]
type_rows = [
    row for row in clone_rows
    if row["phase"] in ("W3-fill", "W4-fill", "W5-fill", "W6-fill")
]
heaviest = max(
    type_rows,
    key=lambda row: float(row["bytes_per_key"] or 0),
    default=None,
)
w1 = {target: first_row(rows_by_target[target], "W1-fill")
      for target in targets}

with open(os.path.join(results_dir, "finding.md"), "w",
          encoding="utf-8") as report:
    report.write("# Allocator memory profile\n\n")
    report.write(f"- Scale: `{scale}` of the specified workload counts; "
                 + ("per-container sizes kept at the specified values "
                    "(`FIXED_SHAPE=1`).\n" if fixed_shape == "1" else
                    "per-container sizes (hash fields, list elements, set "
                    "members, sorted-set members) were scaled too.\n"))
    report.write(f"- Seed: `{seed}`\n")
    report.write("- Process model: "
                 + ("a fresh server process for every workload.\n"
                    if len(group_list) > 1 else
                    "one server process for all workloads, so later "
                    "workloads inherit earlier heap state (for glibc this "
                    "means the W2-1024 peak shows up in every later row).\n"))
    report.write(f"- Settling: RSS is re-sampled until it is stable for up "
                 f"to {settle_max} s before each measurement.\n")
    report.write(f"- Clone jemalloc: `{jemalloc_library}` with "
                 f"`MALLOC_CONF={jemalloc_conf or '(unset)'}`. Redis uses "
                 "its own bundled allocator and defaults, so versions and "
                 "tuning can differ.\n")
    report.write("- Trim rows (`*-trim`): "
                 + ("collected with `malloc_trim(0)` / jemalloc "
                    "`arena.4096.purge` via gdb on the clone and "
                    "`MEMORY PURGE` on Redis.\n" if trim == "1" else
                    "not collected (`TRIM=1` enables them).\n"))
    report.write(f"- Full logical snapshots matched across glibc, jemalloc, "
                 f"and Redis for: {', '.join(sorted(phases))}.\n")
    report.write("- `used_bytes` is the clone's logical payload estimate; "
                 "allocator allocated/resident includes runtime and "
                 "container overhead. glibc resident is the `mallinfo2` "
                 "`arena + hblkhd` estimate, which is heap extent: it does "
                 "not fall when pages are released with madvise, so judge "
                 "what was returned to the OS by `process_rss_bytes`. "
                 "jemalloc uses refreshed `stats.allocated` and "
                 "`stats.resident`.\n")
    report.write("- `fragmentation_ratio` is allocator resident / allocated "
                 "for the clone and Redis's `mem_fragmentation_ratio` for "
                 "the reference, so it is not comparable across targets. "
                 "`rss_over_allocated` is process RSS / allocator allocated "
                 "computed the same way for all three; use it for "
                 "cross-target comparison. `bytes_per_key` uses logical "
                 "used bytes (Redis `used_memory`) / live keys.\n")
    report.write("- Clone accounting tolerance compares the change from "
                 "the per-process baseline: "
                 "`abs(allocator_allocated_bytes - accounting_baseline_bytes "
                 "- used_bytes) <= max(1 MiB, 25% of used_bytes)` for "
                 "populated phases; rows outside that bound are marked "
                 "`investigate` in the CSV.\n")
    if heaviest:
        report.write(f"- Highest clone logical bytes/key among W3-W6 in the "
                     f"glibc run: {heaviest['phase']} at "
                     f"{heaviest['bytes_per_key']} bytes/key. Compare the "
                     "allocator columns in the CSV to quantify container "
                     "overhead.\n")
    if all(w1.values()):
        per_key = []
        for target in targets:
            baseline = int(rows_by_target[target][0]
                           ["allocator_allocated_bytes"])
            held = int(w1[target]["allocator_allocated_bytes"]) - baseline
            per_key.append(f"{target} {held / int(w1[target]['keys']):.0f}")
        report.write("- W1 allocator-allocated bytes/key above the process "
                     "baseline (a 16-byte key plus a 64-byte value is 80): "
                     + ", ".join(per_key) + ".\n")
        logical = float(w1["glibc"]["bytes_per_key"])
        report.write(f"- The clone's logical counter charges {logical:.0f} "
                     f"bytes/key for a W1 entry, i.e. {logical - 80:.0f} "
                     "beyond the 16-byte key and 64-byte value "
                     "(`Object::logicalSize`).\n")
    accounting_issues = [
        (target, row["phase"], row["used_bytes"],
         row["allocator_allocated_bytes"], row["accounting_baseline_bytes"],
         row["accounting_delta_bytes"])
        for target in ("glibc", "jemalloc")
        for row in rows_by_target[target]
        if row["accounting_status"] == "investigate"
    ]
    if accounting_issues:
        report.write("- Accounting tolerance exceeded; investigate these "
                     "measurements:\n")
        for target, phase, used, allocated, baseline, delta in accounting_issues:
            report.write(f"  - {target}, {phase}: used_bytes={used}, "
                         f"allocator_allocated_bytes={allocated}, "
                         f"baseline={baseline}, "
                         f"delta={delta}\n")
        if w1["glibc"]:
            delta = (int(w1["glibc"]["accounting_delta_bytes"])
                     / int(w1["glibc"]["keys"]))
            report.write(f"- W1 and W2 hold only plain strings, yet glibc "
                         f"W1-fill is {delta:.0f} bytes/key over the logical "
                         "counter. So the counter is missing per-entry "
                         "keyspace overhead and allocator size-class "
                         "rounding, not only container costs. W3-W6 "
                         "additionally omit hash/set bucket and node "
                         "storage, deque blocks, and ZSet skip-list nodes "
                         "plus the score index. The CSV records the "
                         "observed delta per phase.\n")
    else:
        report.write("- Allocator allocated bytes were within the documented "
                     "tolerance for every populated clone phase.\n")

    retained = []
    for target in targets:
        final = next((row for row in reversed(rows_by_target[target])
                      if row["phase"].endswith("-free")), None)
        if final:
            retained.append(f"{target} {mib(final['allocator_allocated_bytes'])}"
                            f" MiB ({final['phase']})")
    if retained:
        report.write("- Allocator bytes still allocated after the last free "
                     "phase: " + ", ".join(retained) + ". A clone figure "
                     "that does not depend on the data written points at a "
                     "keyspace structure that FLUSHALL does not shrink.\n")

    report.write("- RSS handed back after freeing (`-free` rows compare with "
                 "the fill before them; `-trim` rows with the free row):\n\n")
    report.write("  | target | phase | RSS before (MiB) | RSS after (MiB) "
                 "| returned |\n  |---|---|---:|---:|---:|\n")
    for target in targets:
        rows = rows_by_target[target]
        for position, row in enumerate(rows):
            phase = row["phase"]
            if position == 0 or not (phase.endswith("-free")
                                     or phase.endswith("-free-trim")):
                continue
            before = int(rows[position - 1]["process_rss_bytes"])
            after = int(row["process_rss_bytes"])
            percent = (before - after) / before * 100 if before else 0.0
            report.write(f"  | {target} | {phase} | {mib(before)} | "
                         f"{mib(after)} | {max(percent, 0.0):.0f}% |\n")
    report.write("\n- Raw phase measurements: "
                 "[allocator_memory.csv](./allocator_memory.csv)\n")
print("Wrote " + combined_path)
print("Wrote " + os.path.join(results_dir, "finding.md"))
PY