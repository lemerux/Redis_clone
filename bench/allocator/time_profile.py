#!/usr/bin/env python3
"""Timing workloads that mirror mem_profile.py.

Two kinds of groups, selected with --workloads:

  W1..W8  Phase durations. The same fills, overwrites, deletes and FLUSHALLs
          as mem_profile.py (its helpers are imported, so the data written is
          identical), but each phase is timed with a monotonic clock instead of
          being followed by a memory snapshot. The Python client is the
          bottleneck here, so compare targets with each other, not against
          absolute numbers.

  BENCH   Per-command latency and throughput measured by redis-benchmark:
          SET/GET at three value sizes plus a list workload (LPUSH, RPUSH,
          LPOP, RPOP, LINDEX, LSET) aimed at the ListValues ring buffer.
          Each test runs twice: "latency" (1 client, no pipelining) and
          "throughput" (--throughput-clients clients, pipeline 16).

          The clone allows one active session per user, so the Python
          connection is closed while redis-benchmark runs and the default is
          a single benchmark connection. Raise --throughput-clients only if
          the clone is changed to allow several sessions for one user.
"""

import argparse
import csv
import os
import random
import subprocess
import sys
import time

import mem_profile as mp

PHASE_FIELDS = ("target", "repeat", "workload", "phase", "operations",
                "seconds", "ops_per_sec")
COMMAND_FIELDS = ("target", "repeat", "test", "mode", "clients", "pipeline",
                  "requests", "rps", "avg_ms", "min_ms", "p50_ms", "p95_ms",
                  "p99_ms", "max_ms", "note")

WARMUP_REQUESTS = 2000
PRELOAD_SLACK = 2000          # more than clients * pipeline, so pops never run dry
LIST_VALUE_SIZE = 64
VALUE_SIZES = (64, 256, 1024)


def modes(args):
    # name, clients, pipeline, requests key
    return (
        ("throughput", args.throughput_clients, 16, "throughput_requests"),
        ("latency", 1, 1, "latency_requests"),
    )


def connect(args, attempts=40):
    """Connect and authenticate, waiting out a session that is still closing.

    The clone rejects AUTH with "user already has an active session" until the
    server has noticed that the previous connection of that user went away.
    """
    last = None
    for _ in range(attempts):
        try:
            return mp.wait_for_server(args.host, args.port, args.kind,
                                      args.username, args.password)
        except RuntimeError as error:
            if "active session" not in str(error):
                raise
            last = error
            time.sleep(0.25)
    raise last


# --------------------------------------------------------------------------
# Phase durations (W1-W8)
# --------------------------------------------------------------------------

class PhaseTimer:
    def __init__(self, args):
        self.args = args
        self.rows = []

    def run(self, workload, phase, operations, action):
        start = time.perf_counter()
        action()
        elapsed = time.perf_counter() - start
        rate = f"{operations / elapsed:.1f}" if operations > 1 and elapsed > 0 else ""
        self.rows.append({
            "target": self.args.allocator,
            "repeat": self.args.repeat,
            "workload": workload,
            "phase": phase,
            "operations": operations,
            "seconds": f"{elapsed:.6f}",
            "ops_per_sec": rate,
        })
        print(f"{self.args.allocator}: {phase}: {elapsed:.3f}s "
              f"({operations} ops)", file=sys.stderr)

    def flush(self, workload, phase, client):
        # One command; FLUSHALL is timed until its reply, so lazy/background
        # freeing after the reply is not included.
        self.run(workload, phase, 1, lambda: client.command("FLUSHALL"))


def phases_w1(t, client, args, shape):
    n = shape["keys_1m"]
    t.run("W1", "W1-fill", n, lambda: mp.fill_strings(client, n, 64, args.seed))
    t.flush("W1", "W1-free", client)


def phases_w2(t, client, args, shape):
    n = shape["keys_1m"]
    for size in (256, 1024):
        t.run("W2", f"W2-{size}-fill", n,
              lambda size=size: mp.fill_strings(client, n, size, args.seed))
        t.flush("W2", f"W2-{size}-free", client)


def phases_w3(t, client, args, shape):
    n = shape["hash_count"]
    t.run("W3", "W3-fill", n,
          lambda: mp.fill_hashes(client, n, shape["hash_fields"], args.seed))
    t.flush("W3", "W3-free", client)


def phases_w4(t, client, args, shape):
    n = shape["list_count"]
    t.run("W4", "W4-fill", n, lambda: mp.fill_collections(
        client, "list", n, shape["list_members"], args.seed))
    t.flush("W4", "W4-free", client)


def phases_w5(t, client, args, shape):
    n = shape["set_count"]
    t.run("W5", "W5-fill", n, lambda: mp.fill_collections(
        client, "set", n, shape["set_members"], args.seed))
    t.flush("W5", "W5-free", client)


def phases_w6(t, client, args, shape):
    count, members = shape["zset_count"], shape["zset_members"]
    commands = count * ((members + 199) // 200)   # fill_zsets sends 200 per ZADD
    t.run("W6", "W6-fill", commands,
          lambda: mp.fill_zsets(client, count, members))
    t.flush("W6", "W6-free", client)


def phases_w7(t, client, args, shape):
    seed = args.seed
    n = shape["keys_1m"]
    t.run("W7", "W7-fill", n, lambda: mp.fill_strings(client, n, 64, seed))
    live_ids = list(range(n))
    generator = random.Random(seed)
    for number, size in enumerate(mp.W7_OVERWRITE_SIZES, start=1):
        overwritten = generator.sample(range(n), n // 2)
        t.run("W7", f"W7-round{number}-overwrite", len(overwritten),
              lambda: mp.fill_strings(client, n, size, seed, number,
                                      overwritten))
        deleted = generator.sample(live_ids, max(1, len(live_ids) * 3 // 10))
        t.run("W7", f"W7-round{number}-delete", len(deleted),
              lambda: mp.generated_pipeline(
                  client, len(deleted),
                  lambda p: ("DEL", mp.key("s", deleted[p]))))
        gone = set(deleted)
        live_ids = [i for i in live_ids if i not in gone]
        t.run("W7", f"W7-round{number}-refill", len(deleted),
              lambda: mp.fill_strings(client, n, 64, seed, number + 10,
                                      deleted))
        live_ids.extend(deleted)
        live_ids.sort()
    t.flush("W7", "W7-free", client)


def phases_w8(t, client, args, shape):
    n = shape["hash_count"]
    fields = shape["hash_fields"]
    t.run("W8", "W8-del-fill", n,
          lambda: mp.fill_hashes(client, n, fields, args.seed))
    hash_keys = [mp.key("h", i) for i in range(n)]
    t.run("W8", "W8-del-free", n, lambda: mp.run_commands(
        client, (("DEL", *hash_keys[o:o + 128]) for o in range(0, n, 128))))
    t.run("W8", "W8-flush-fill", n,
          lambda: mp.fill_hashes(client, n, fields, args.seed))
    t.flush("W8", "W8-flushall-free", client)


PHASE_WORKLOADS = {
    "W1": phases_w1, "W2": phases_w2, "W3": phases_w3, "W4": phases_w4,
    "W5": phases_w5, "W6": phases_w6, "W7": phases_w7, "W8": phases_w8,
}


# --------------------------------------------------------------------------
# redis-benchmark (BENCH)
# --------------------------------------------------------------------------

def preload_strings(keyspace, size):
    def prep(client, _needed):
        # redis-benchmark names keys key:<12-digit zero padded random int>.
        mp.generated_pipeline(
            client, keyspace,
            lambda i: ("SET", f"key:{i:012d}", "x" * size))
    return prep


def preload_list(name, length_of):
    """Fill list `name` with length_of(needed) values of LIST_VALUE_SIZE."""
    def prep(client, needed):
        count = length_of(needed)
        value = "x" * LIST_VALUE_SIZE
        batch = 100
        mp.generated_pipeline(
            client, (count + batch - 1) // batch,
            lambda i: ("RPUSH", name,
                       *([value] * min(batch, count - i * batch))))
    return prep


def no_prep(_client, _needed):
    return None


def list_checks(name):
    """After a pop test, an emptied list means the numbers are meaningless."""
    def check(client):
        return "" if client.command("LLEN", name) > 0 else "list emptied"
    return check


def bench_specs(shape_args):
    keyspace = mp.sized(100_000, shape_args.scale)
    list_len = max(100, mp.sized(10_000, shape_args.scale))
    mid = list_len // 2
    value = "x" * LIST_VALUE_SIZE
    specs = []

    for size in VALUE_SIZES:
        specs.append({"test": f"SET-{size}", "words": ["-t", "set"],
                      "extra": ["-r", str(keyspace), "-d", str(size)],
                      "prep": no_prep})
        specs.append({"test": f"GET-{size}", "words": ["-t", "get"],
                      "extra": ["-r", str(keyspace), "-d", str(size)],
                      "prep": preload_strings(keyspace, size)})

    # Pushes grow one deep list; the "many" variants spread over small lists.
    specs += [
        {"test": "LPUSH", "words": ["LPUSH", "dq", value], "prep": no_prep},
        {"test": "RPUSH", "words": ["RPUSH", "dq", value], "prep": no_prep},
        {"test": "LPUSH-many", "words": ["LPUSH", "lm:__rand_int__", value],
         "extra": ["-r", "10000"], "prep": no_prep},
        {"test": "RPUSH-many", "words": ["RPUSH", "lm:__rand_int__", value],
         "extra": ["-r", "10000"], "prep": no_prep},
    ]
    # Pops need at least as many elements as requests will be issued.
    for command in ("LPOP", "RPOP"):
        specs.append({"test": command, "words": [command, "dq"],
                      "prep": preload_list("dq", lambda needed: needed),
                      "check": list_checks("dq")})
    # Index/set: fixed indices only. __rand_int__ is zero padded and Redis
    # rejects leading zeros in integer arguments.
    deep = preload_list("dq", lambda _needed: list_len)
    for label, index in (("head", 0), ("mid", mid), ("tail", -1)):
        specs.append({"test": f"LINDEX-{label}",
                      "words": ["LINDEX", "dq", str(index)], "prep": deep})
    for label, index in (("head", 0), ("mid", mid), ("tail", -1)):
        specs.append({"test": f"LSET-{label}-samelen",
                      "words": ["LSET", "dq", str(index), value], "prep": deep})
    return specs


def parse_benchmark_csv(text):
    header = data = None
    for row in csv.reader(line for line in text.splitlines() if line.strip()):
        if row[0] == "test":
            header = row
        else:
            data = row
    if header is None or data is None:
        raise RuntimeError("could not parse redis-benchmark --csv output: "
                           + repr(text[-200:]))
    return dict(zip(header, data))


def run_benchmark(args, spec, clients, pipeline, requests):
    command = [args.redis_benchmark, "-h", args.host, "-p", str(args.port),
               "-c", str(clients), "-P", str(pipeline), "-n", str(requests),
               "--csv"]
    if args.kind == "clone":
        command += ["--user", args.username, "-a", args.password]
    command += spec.get("extra", []) + spec["words"]
    for _ in range(40):
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=900, check=False)
        output = result.stderr + result.stdout
        if result.returncode != 0 and "active session" in output:
            time.sleep(0.25)   # previous session is still being torn down
            continue
        break
    if result.returncode != 0:
        raise RuntimeError(f"redis-benchmark failed for {spec['test']}: "
                           + output.strip()[-300:])
    return parse_benchmark_csv(result.stdout)


def bench(client, args):
    requests = {
        "throughput_requests": mp.sized(200_000, args.scale),
        "latency_requests": mp.sized(20_000, args.scale),
    }
    rows = []
    for spec in bench_specs(args):
        for mode, clients, pipeline, key in modes(args):
            count = requests[key]
            client.command("FLUSHALL")
            spec["prep"](client, count + WARMUP_REQUESTS + PRELOAD_SLACK)
            # Free the user's single session for redis-benchmark.
            client.close()
            try:
                run_benchmark(args, spec, clients, pipeline, WARMUP_REQUESTS)
                result = run_benchmark(args, spec, clients, pipeline, count)
            finally:
                client = connect(args)
            note = spec["check"](client) if "check" in spec else ""
            if "p99_latency_ms" not in result:
                note = (note + " no percentile columns (old redis-benchmark)"
                        ).strip()
            rows.append({
                "target": args.allocator, "repeat": args.repeat,
                "test": spec["test"], "mode": mode, "clients": clients,
                "pipeline": pipeline, "requests": count,
                "rps": result.get("rps", ""),
                "avg_ms": result.get("avg_latency_ms", ""),
                "min_ms": result.get("min_latency_ms", ""),
                "p50_ms": result.get("p50_latency_ms", ""),
                "p95_ms": result.get("p95_latency_ms", ""),
                "p99_ms": result.get("p99_latency_ms", ""),
                "max_ms": result.get("max_latency_ms", ""),
                "note": note,
            })
            print(f"{args.allocator}: {spec['test']} {mode}: "
                  f"{result.get('rps')} rps, p99 "
                  f"{result.get('p99_latency_ms', 'n/a')} ms {note}",
                  file=sys.stderr)
    client.command("FLUSHALL")
    return rows, client


# --------------------------------------------------------------------------

def write_csv(path, fields, rows):
    with open(path, "w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def parse_groups(value, parser):
    allowed = list(PHASE_WORKLOADS) + ["BENCH"]
    if value.strip().upper() == "ALL":
        return allowed
    selected = [v.strip().upper() for v in value.split(",") if v.strip()]
    if not selected or any(v not in allowed for v in selected):
        parser.error("--workloads must be ALL or a comma list of "
                     + ",".join(allowed))
    return selected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--kind", choices=("clone", "redis"), required=True)
    parser.add_argument("--allocator", required=True)
    parser.add_argument("--username", default="bench")
    parser.add_argument("--password", default="benchmark-password")
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--repeat", type=int, default=1,
                        help="repeat number, recorded in the CSV")
    parser.add_argument("--workloads", default="ALL",
                        help="ALL or a comma list of W1..W8,BENCH")
    parser.add_argument("--fixed-shape", action="store_true")
    parser.add_argument("--redis-benchmark", default="redis-benchmark")
    parser.add_argument("--throughput-clients", type=int, default=1,
                        help="redis-benchmark connections in throughput mode "
                             "(the clone allows one session per user)")
    parser.add_argument("--output-prefix", required=True,
                        help="writes <prefix>.phases.csv and "
                             "<prefix>.commands.csv")
    args = parser.parse_args()
    if not 0 < args.scale <= 1:
        parser.error("--scale must be in (0, 1]")
    groups = parse_groups(args.workloads, parser)

    client = connect(args)
    phase_rows, command_rows = [], []
    try:
        timer = PhaseTimer(args)
        shape = mp.workload_shape(args)
        for group in groups:
            if group == "BENCH":
                bench_rows, client = bench(client, args)
                command_rows += bench_rows
            else:
                PHASE_WORKLOADS[group](timer, client, args, shape)
        phase_rows = timer.rows
    finally:
        client.close()

    if phase_rows:
        write_csv(args.output_prefix + ".phases.csv", PHASE_FIELDS, phase_rows)
    if command_rows:
        write_csv(args.output_prefix + ".commands.csv", COMMAND_FIELDS,
                  command_rows)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"time_profile: {error}", file=sys.stderr)
        sys.exit(1)