#!/bin/sh
# Timing profile: clone on glibc, clone on jemalloc, reference Redis.
# Mirrors mem_profile.sh and reuses mem_profile.py's workload helpers.
#
# Produces, in RESULTS_DIR:
#   time_phases.csv    duration of every W1-W8 phase (median over REPEATS)
#   time_commands.csv  redis-benchmark latency/throughput (median over REPEATS)
#   time_finding.md    summary tables with each target relative to glibc
#
# Environment:
#   SCALE=1            fraction of the specified workload counts, in (0, 1]
#   SEED=20261109      payload seed
#   REPEATS=3          fresh-server repetitions; the summary uses the median
#   FIXED_SHAPE=0      1 scales only container counts (see mem_profile.sh)
#   THROUGHPUT_CLIENTS=1  redis-benchmark connections in throughput mode. The
#                      clone allows one active session per user, so keep 1
#                      unless that limit is lifted.
#   GROUPS_TO_RUN      default "W1 W2 W3 W4 W5 W6 W7 W8 BENCH"
#   SERVER_CPU=        CPU list for the server (taskset -c), e.g. 2
#   CLIENT_CPU=        CPU list for the load generators, e.g. 3
#                      Pinning them to different cores makes runs much less
#                      noisy. Both are ignored when taskset is missing.
#   JEMALLOC_CONF      MALLOC_CONF for the preloaded jemalloc; default
#                      background_thread:true (matches Redis's default).
set -eu

# shellcheck disable=SC1007
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1007
repo_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
source_dir="$repo_dir/src"
build_dir=${BUILD_DIR:-/tmp/redis-clone-allocator-mem-build}
results_dir=${RESULTS_DIR:-"$repo_dir/bench/results/allocator_time"}
scale=${SCALE:-1}
seed=${SEED:-20261109}
repeats=${REPEATS:-3}
python=${PYTHON:-python3}
fixed_shape=${FIXED_SHAPE:-0}
groups=${GROUPS_TO_RUN:-"W1 W2 W3 W4 W5 W6 W7 W8 BENCH"}
jemalloc_conf=${JEMALLOC_CONF-background_thread:true}

for tool in cmake "$python" redis-server redis-benchmark ldconfig awk; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "required tool not found: $tool" >&2
        exit 1
    }
done

server_prefix=
client_prefix=
if command -v taskset >/dev/null 2>&1; then
    if [ -n "${SERVER_CPU:-}" ]; then
        server_prefix="taskset -c $SERVER_CPU"
    fi
    if [ -n "${CLIENT_CPU:-}" ]; then
        client_prefix="taskset -c $CLIENT_CPU"
    fi
elif [ -n "${SERVER_CPU:-}${CLIENT_CPU:-}" ]; then
    echo "taskset not found; SERVER_CPU/CLIENT_CPU ignored" >&2
fi

jemalloc_library=$(ldconfig -p |
    awk '/libjemalloc\.so(\.|$)/ { print $NF; exit }')
if [ -z "$jemalloc_library" ]; then
    echo "libjemalloc was not found by ldconfig" >&2
    exit 1
fi
echo "clone jemalloc: $jemalloc_library (MALLOC_CONF=${jemalloc_conf:-unset})" >&2

throughput_clients=${THROUGHPUT_CLIENTS:-1}
export THROUGHPUT_CLIENTS=$throughput_clients
extra_args="--scale $scale --seed $seed --throughput-clients $throughput_clients"
[ "$fixed_shape" = 1 ] && extra_args="$extra_args --fixed-shape"

cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target redis-server --parallel
server="$build_dir/redis-server"
mkdir -p "$results_dir"
temporary_dir=$(mktemp -d)
server_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$temporary_dir"
}
trap cleanup EXIT HUP INT TERM

users_file="$temporary_dir/users.conf"
printf '%s\n' 'bench benchmark-password' > "$users_file"

available_port() {
    "$python" -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

# time_profile.py waits for the port to accept connections before it starts
# the clock, so server start-up is not measured.
run_profile() {
    kind=$1
    allocator=$2
    port=$3
    group=$4
    rep=$5
    # $extra_args and $client_prefix are intentionally unquoted.
    # shellcheck disable=SC2086
    $client_prefix "$python" "$script_dir/time_profile.py" \
        --kind "$kind" --allocator "$allocator" --port "$port" \
        --workloads "$group" --repeat "$rep" $extra_args \
        --output-prefix "$temporary_dir/$allocator.$group.$rep"
}

run_target() {
    allocator=$1
    for rep in $(seq 1 "$repeats"); do
        for group in $groups; do
            port=$(available_port)
            case "$allocator" in
                jemalloc)
                    # shellcheck disable=SC2086
                    env LD_PRELOAD="$jemalloc_library" \
                        MALLOC_CONF="$jemalloc_conf" \
                        $server_prefix "$server" "$port" "$users_file" \
                        >/dev/null 2>&1 &
                    ;;
                glibc)
                    # shellcheck disable=SC2086
                    env -u LD_PRELOAD -u MALLOC_CONF \
                        $server_prefix "$server" "$port" "$users_file" \
                        >/dev/null 2>&1 &
                    ;;
                redis)
                    # shellcheck disable=SC2086
                    $server_prefix redis-server --bind 127.0.0.1 \
                        --port "$port" --protected-mode no --save "" \
                        --appendonly no --loglevel warning \
                        --daemonize no --dir "$temporary_dir" \
                        >/dev/null 2>&1 &
                    ;;
            esac
            server_pid=$!
            if [ "$allocator" = redis ]; then
                run_profile redis redis "$port" "$group" "$rep"
            else
                run_profile clone "$allocator" "$port" "$group" "$rep"
            fi
            kill "$server_pid"
            wait "$server_pid" 2>/dev/null || true
            server_pid=
        done
    done
}

run_target glibc
run_target jemalloc
run_target redis

"$python" - "$temporary_dir" "$results_dir" "$scale" "$seed" "$repeats" \
    "$groups" "$fixed_shape" "$jemalloc_conf" "$jemalloc_library" \
    "${SERVER_CPU:-}" "${CLIENT_CPU:-}" <<'PY'
import csv
import glob
import os
import statistics
import sys

(temporary_dir, results_dir, scale, seed, repeats, groups, fixed_shape,
 jemalloc_conf, jemalloc_library, server_cpu, client_cpu) = sys.argv[1:]
targets = ("glibc", "jemalloc", "redis")
clients = os.environ.get("THROUGHPUT_CLIENTS", "1")


def load(suffix):
    rows = []
    for path in sorted(glob.glob(os.path.join(temporary_dir, "*." + suffix))):
        with open(path, newline="", encoding="utf-8") as source:
            rows.extend(csv.DictReader(source))
    return rows


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def median_of(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def spread(values):
    values = [v for v in values if v is not None]
    if len(values) < 2 or statistics.median(values) == 0:
        return None
    return (max(values) - min(values)) / statistics.median(values) * 100


def fmt(value, digits=3):
    return "" if value is None else f"{value:.{digits}f}"


phases = load("phases.csv")
commands = load("commands.csv")

# ---- phases: median seconds per (target, phase) ---------------------------
phase_order = []
seconds = {}
operations = {}
for row in phases:
    key = (row["target"], row["phase"])
    if row["phase"] not in phase_order:
        phase_order.append(row["phase"])
    seconds.setdefault(key, []).append(number(row["seconds"]))
    operations[row["phase"]] = row["operations"]

phase_summary = []
for phase in phase_order:
    entry = {"phase": phase, "operations": operations[phase]}
    for target in targets:
        values = seconds.get((target, phase), [])
        entry[target] = median_of(values)
        entry[target + "_spread"] = spread(values)
    phase_summary.append(entry)

with open(os.path.join(results_dir, "time_phases.csv"), "w", newline="",
          encoding="utf-8") as output:
    writer = csv.writer(output)
    writer.writerow(["phase", "operations"]
                    + [f"{t}_median_s" for t in targets]
                    + [f"{t}_spread_pct" for t in targets])
    for entry in phase_summary:
        writer.writerow([entry["phase"], entry["operations"]]
                        + [fmt(entry[t], 6) for t in targets]
                        + [fmt(entry[t + "_spread"], 1) for t in targets])

# ---- commands: median rps / latency per (target, test, mode) --------------
command_keys = []
cells = {}
notes = set()
for row in commands:
    key = (row["test"], row["mode"])
    if key not in command_keys:
        command_keys.append(key)
    cell = cells.setdefault((row["target"],) + key,
                            {"rps": [], "p50": [], "p99": []})
    cell["rps"].append(number(row["rps"]))
    cell["p50"].append(number(row["p50_ms"]))
    cell["p99"].append(number(row["p99_ms"]))
    if row["note"]:
        notes.add(f"{row['target']} {row['test']} {row['mode']}: {row['note']}")

command_summary = []
for test, mode in command_keys:
    entry = {"test": test, "mode": mode}
    for target in targets:
        cell = cells.get((target, test, mode))
        entry[target + "_rps"] = median_of(cell["rps"]) if cell else None
        entry[target + "_p50"] = median_of(cell["p50"]) if cell else None
        entry[target + "_p99"] = median_of(cell["p99"]) if cell else None
        entry[target + "_rps_spread"] = spread(cell["rps"]) if cell else None
    command_summary.append(entry)

with open(os.path.join(results_dir, "time_commands.csv"), "w", newline="",
          encoding="utf-8") as output:
    writer = csv.writer(output)
    header = ["test", "mode"]
    for metric in ("rps", "p50_ms", "p99_ms", "rps_spread_pct"):
        header += [f"{t}_{metric}" for t in targets]
    writer.writerow(header)
    for entry in command_summary:
        line = [entry["test"], entry["mode"]]
        line += [fmt(entry[t + "_rps"], 1) for t in targets]
        line += [fmt(entry[t + "_p50"], 4) for t in targets]
        line += [fmt(entry[t + "_p99"], 4) for t in targets]
        line += [fmt(entry[t + "_rps_spread"], 1) for t in targets]
        writer.writerow(line)


def ratio(value, base):
    if value is None or not base:
        return ""
    return f"{value / base:.2f}x"


with open(os.path.join(results_dir, "time_finding.md"), "w",
          encoding="utf-8") as report:
    report.write("# Allocator timing profile\n\n")
    report.write(f"- Scale `{scale}`, seed `{seed}`, "
                 f"{repeats} repeat(s); every number is the median over "
                 "fresh server processes (a new process per group and "
                 "repeat).\n")
    report.write("- Clone jemalloc: "
                 f"`{jemalloc_library}` with "
                 f"`MALLOC_CONF={jemalloc_conf or '(unset)'}`. Redis uses its "
                 "own bundled allocator.\n")
    report.write("- CPU pinning: "
                 + (f"server `{server_cpu or 'none'}`, load generator "
                    f"`{client_cpu or 'none'}`.\n"
                    if server_cpu or client_cpu else
                    "none. Set `SERVER_CPU` and `CLIENT_CPU` for steadier "
                    "numbers.\n"))
    report.write("- Phase durations are timed around the Python pipelined "
                 "client, so the client is the bottleneck. Compare targets "
                 "with each other; do not read them as server limits. "
                 "`FLUSHALL` is timed until its reply only.\n")
    report.write("- Latency mode is 1 client, no pipelining. Throughput "
                 f"mode is {clients} client(s) with pipeline 16; its latency "
                 "figures are per pipelined batch and are not comparable to "
                 "latency mode. `spread` is (max - min) / median across "
                 "repeats; treat differences smaller than it as noise.\n")
    report.write("- Redis is the reference target and is a different "
                 "program. Compare glibc with jemalloc to isolate the "
                 "allocator, and the clone with Redis to see the gap.\n")
    for note in sorted(notes):
        report.write(f"- WARNING {note}\n")

    if phase_summary:
        report.write("\n## Phase duration (seconds, median)\n\n")
        report.write("| phase | ops | glibc | jemalloc | redis "
                     "| jemalloc/glibc | redis/glibc |\n"
                     "|---|---:|---:|---:|---:|---:|---:|\n")
        for e in phase_summary:
            report.write(
                f"| {e['phase']} | {e['operations']} | {fmt(e['glibc'])} "
                f"| {fmt(e['jemalloc'])} | {fmt(e['redis'])} "
                f"| {ratio(e['jemalloc'], e['glibc'])} "
                f"| {ratio(e['redis'], e['glibc'])} |\n")

    for mode in ("latency", "throughput"):
        rows = [e for e in command_summary if e["mode"] == mode]
        if not rows:
            continue
        title = ("Per-command latency, 1 client no pipelining"
                 if mode == "latency" else
                 f"Throughput, {clients} client(s) pipeline 16")
        report.write(f"\n## {title}\n\n")
        report.write("| test | rps glibc | rps jemalloc | rps redis "
                     "| p50 ms glibc | p50 ms jemalloc | p50 ms redis "
                     "| p99 ms glibc | p99 ms jemalloc | p99 ms redis |\n"
                     "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for e in rows:
            report.write(
                f"| {e['test']} "
                + " ".join(f"| {fmt(e[t + '_rps'], 0)} " for t in targets)
                + " ".join(f"| {fmt(e[t + '_p50'], 3)} " for t in targets)
                + " ".join(f"| {fmt(e[t + '_p99'], 3)} " for t in targets)
                + "|\n")
    report.write("\n- Raw data: [time_phases.csv](./time_phases.csv), "
                 "[time_commands.csv](./time_commands.csv)\n")
print("Wrote " + os.path.join(results_dir, "time_phases.csv"))
print("Wrote " + os.path.join(results_dir, "time_commands.csv"))
print("Wrote " + os.path.join(results_dir, "time_finding.md"))
PY