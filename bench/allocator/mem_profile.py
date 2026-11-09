#!/usr/bin/env python3
"""Pipelined allocator-memory workloads and type-aware logical snapshots."""

import argparse
import csv
import hashlib
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import time

ACCOUNTING_ABSOLUTE_TOLERANCE = 1024 * 1024


def encode_command(arguments):
    encoded = [
        argument if isinstance(argument, bytes) else str(argument).encode()
        for argument in arguments
    ]
    parts = [b"*" + str(len(encoded)).encode() + b"\r\n"]
    for value in encoded:
        parts.extend(
            (b"$" + str(len(value)).encode() + b"\r\n", value, b"\r\n")
        )
    return b"".join(parts)


class Client:
    def __init__(self, host, port):
        self.connection = socket.create_connection((host, port), timeout=10)
        self.connection.settimeout(120)
        self.stream = self.connection.makefile("rb")

    def _read_exact(self, size):
        value = self.stream.read(size)
        if len(value) != size:
            raise RuntimeError("unexpected EOF reading RESP response")
        return value

    def _read_line(self):
        value = self.stream.readline()
        if not value.endswith(b"\r\n"):
            raise RuntimeError("invalid RESP line")
        return value[:-2]

    def read_response(self):
        marker = self._read_exact(1)
        if marker == b"+":
            return self._read_line()
        if marker == b"-":
            raise RuntimeError("server returned RESP error: " +
                               self._read_line().decode(errors="replace"))
        if marker == b":":
            return int(self._read_line())
        if marker == b"$":
            length = int(self._read_line())
            if length == -1:
                return None
            value = self._read_exact(length)
            if self._read_exact(2) != b"\r\n":
                raise RuntimeError("invalid RESP bulk string terminator")
            return value
        if marker == b"*":
            length = int(self._read_line())
            if length == -1:
                return None
            return [self.read_response() for _ in range(length)]
        raise RuntimeError("unknown RESP response marker: " + repr(marker))

    def command(self, *arguments):
        self.connection.sendall(encode_command(arguments))
        return self.read_response()

    def pipeline(self, commands, batch_size=128):
        batch = []
        for command in commands:
            batch.append(command)
            if len(batch) == batch_size:
                self._send_batch(batch)
                batch.clear()
        if batch:
            self._send_batch(batch)

    def _send_batch(self, commands):
        self.connection.sendall(b"".join(encode_command(item) for item in commands))
        for _ in commands:
            self.read_response()

    def close(self):
        self.stream.close()
        self.connection.close()


def parse_info(text):
    info = {}
    for line in text.splitlines():
        if not line or line.startswith("#") or ":" not in line:
            continue
        key, value = line.split(":", 1)
        info[key] = value
    return info


def read_process_rss(pid):
    with open(f"/proc/{pid}/status", encoding="ascii") as status:
        for line in status:
            if line.startswith("VmRSS:"):
                fields = line.split()
                if len(fields) != 3 or fields[2] != "kB":
                    raise RuntimeError("could not parse VmRSS from proc status")
                return int(fields[1]) * 1024
    raise RuntimeError(f"VmRSS is missing from /proc/{pid}/status")


def settle_rss(pid, stable_seconds, max_seconds,
               interval=0.25, tolerance=256 * 1024):
    """Wait until process RSS stops moving.

    Deferred frees, lazy FLUSHALL work, and allocator decay (jemalloc purges
    dirty pages over seconds) can all still be in flight right after a
    command returns. Sampling immediately measures that timing, not the
    allocator.
    """
    if max_seconds <= 0:
        return
    deadline = time.monotonic() + max_seconds
    anchor = read_process_rss(pid)
    stable_since = time.monotonic()
    while True:
        time.sleep(interval)
        now = time.monotonic()
        current = read_process_rss(pid)
        if abs(current - anchor) > tolerance:
            anchor = current
            stable_since = now
        if now - stable_since >= stable_seconds or now >= deadline:
            return


_warned = set()


def warn_once(message):
    if message not in _warned:
        _warned.add(message)
        print("mem_profile: " + message, file=sys.stderr)


def trim_allocator(client, args):
    """Best effort: ask the allocator to hand free memory back to the OS.

    Redis: MEMORY PURGE. glibc clone: malloc_trim(0) through gdb. jemalloc
    clone: arena.<all>.purge through gdb. gdb needs ptrace permission on the
    server process (run as root or set kernel.yama.ptrace_scope=0). Returns
    True only when a trim actually ran, so callers add a row only then.
    """
    if args.kind == "redis":
        try:
            client.command("MEMORY", "PURGE")
        except RuntimeError as error:
            warn_once(f"trim skipped: MEMORY PURGE failed ({error})")
            return False
        return True

    expression = {
        "glibc": "call (int)malloc_trim(0)",
        "jemalloc": 'call (int)mallctl("arena.4096.purge", 0, 0, 0, 0)',
    }.get(args.allocator)
    if expression is None or shutil.which("gdb") is None:
        warn_once("trim skipped: gdb is missing or the allocator is "
                  "unsupported")
        return False
    try:
        result = subprocess.run(
            ["gdb", "-p", str(args.pid), "-batch", "-ex", expression],
            capture_output=True, text=True, timeout=120, check=False,
        )
    except subprocess.TimeoutExpired:
        warn_once("trim skipped: gdb timed out")
        return False
    expected = "$1 = 0" if args.allocator == "jemalloc" else "$1 ="
    if result.returncode != 0 or expected not in result.stdout:
        lines = (result.stderr or result.stdout).strip().splitlines()
        warn_once("trim skipped: gdb could not call the allocator ("
                  + (lines[-1] if lines else "no output")
                  + "); check ptrace permissions "
                  "(kernel.yama.ptrace_scope)")
        return False
    return True


def read_integer(info, *keys):
    for key in keys:
        value = info.get(key)
        if value is not None:
            try:
                return int(value)
            except ValueError as error:
                raise RuntimeError(f"invalid INFO integer {key}={value!r}") from error
    return None


def metrics(client, args, phase, baseline_allocated=None):
    info = parse_info(client.command("INFO").decode(errors="replace"))
    keys = read_integer(info, "keys")
    if keys is None:
        db0 = info.get("db0", "")
        for field in db0.split(","):
            if field.startswith("keys="):
                keys = int(field[5:])
                break
    used = read_integer(info, "used_bytes", "used_memory")
    allocated = read_integer(info, "allocator_allocated_bytes",
                             "allocator_allocated", "used_memory")
    resident = read_integer(info, "allocator_resident_bytes",
                            "allocator_resident", "used_memory_rss")
    rss = read_process_rss(args.pid)
    if keys is None:
        keys = 0

    if args.kind == "clone":
        if used is None or allocated is None or resident is None:
            raise RuntimeError("clone INFO lacks required memory metrics")
        allocator = info.get("allocator", args.allocator)
        fragmentation = resident / allocated if allocated else 0.0
        if baseline_allocated is None:
            baseline_allocated = allocated
        accounting_delta = allocated - baseline_allocated - used
        accounting_tolerance = max(ACCOUNTING_ABSOLUTE_TOLERANCE, used // 4)
        accounting_status = (
            "not_applicable"
            if keys == 0
            else (
                "within_tolerance"
                if abs(accounting_delta) <= accounting_tolerance
                else "investigate"
            )
        )
    else:
        used = read_integer(info, "used_memory")
        if used is None:
            raise RuntimeError("Redis INFO lacks used_memory")
        if resident is None:
            resident = read_integer(info, "used_memory_rss")
        if allocated is None:
            allocated = used
        fragmentation_value = info.get("mem_fragmentation_ratio")
        fragmentation = (
            float(fragmentation_value)
            if fragmentation_value is not None
            else (rss / used if used else 0.0)
        )
        allocator = info.get("mem_allocator", "redis")
        baseline_allocated = ""
        accounting_delta = ""
        accounting_tolerance = ""
        accounting_status = "not_applicable"

    return {
        "target": args.allocator,
        "workload": phase.split("-", 1)[0],
        "phase": phase,
        "allocator": allocator,
        "keys": keys,
        "used_bytes": used,
        "allocator_allocated_bytes": allocated,
        "allocator_resident_bytes": resident,
        "process_rss_bytes": rss,
        "fragmentation_ratio": f"{fragmentation:.6f}",
        "rss_over_allocated": (f"{rss / allocated:.6f}"
                               if allocated else "0.000000"),
        "bytes_per_key": f"{(used / keys):.3f}" if keys else "",
        "accounting_baseline_bytes": baseline_allocated,
        "accounting_delta_bytes": accounting_delta,
        "accounting_tolerance_bytes": accounting_tolerance,
        "accounting_status": accounting_status,
        "snapshot_sha256": "",
    }


def sized(count, scale):
    return max(1, int(round(count * scale)))


def key(prefix, index):
    return prefix + f"{index:015d}"


def payload(size, seed, index, round_number=0):
    prefix = f"{seed:08x}{index:015d}{round_number:02d}"
    return (prefix + "x" * size)[:size]


def run_commands(client, commands):
    client.pipeline(commands)


def generated_pipeline(client, count, make_command, batch_size=128):
    commands = []
    for index in range(count):
        commands.append(make_command(index))
        if len(commands) >= batch_size:
            run_commands(client, commands)
            commands.clear()
    if commands:
        run_commands(client, commands)


def fill_strings(client, count, size, seed, round_number=0, ids=None):
    indices = range(count) if ids is None else ids
    commands = []
    for index in indices:
        commands.append(("SET", key("s", index),
                         payload(size, seed, index, round_number)))
        if len(commands) == 128:
            run_commands(client, commands)
            commands.clear()
    if commands:
        run_commands(client, commands)


def fill_hashes(client, count, fields, seed):
    def command(index):
        args = ["HSET", key("h", index)]
        for field in range(fields):
            args.extend((f"f{field:04d}", payload(32, seed, index * fields + field)))
        return args
    generated_pipeline(client, count, command)


def fill_collections(client, kind, count, members, seed):
    command_name = {"list": "RPUSH", "set": "SADD"}[kind]
    prefix = {"list": "l", "set": "u"}[kind]

    def command(index):
        values = [f"m{index:010d}{member:05d}" for member in range(members)]
        return [command_name, key(prefix, index), *values]
    generated_pipeline(client, count, command)


def fill_zsets(client, count, members):
    commands = []
    for index in range(count):
        name = key("z", index)
        for offset in range(0, members, 200):
            command = ["ZADD", name]
            for member in range(offset, min(offset + 200, members)):
                command.extend((member, f"m{member:015d}"))
            commands.append(command)
            if len(commands) == 128:
                run_commands(client, commands)
                commands.clear()
    if commands:
        run_commands(client, commands)


def add_field(digest, value):
    if isinstance(value, (str, int)):
        value = str(value).encode()
    digest.update(struct.pack(">Q", len(value)))
    digest.update(value)


def snapshot(client, datasets):
    expected_keys = sum(count for _, _, count in datasets)
    if client.command("DBSIZE") != expected_keys:
        raise RuntimeError("DBSIZE differs from expected logical scan size")
    digest = hashlib.sha256()
    commands = {
        "string": lambda item: ("GET", item),
        "hash": lambda item: ("HGETALL", item),
        "list": lambda item: ("LRANGE", item, 0, -1),
        "set": lambda item: ("SMEMBERS", item),
    }
    scan_batch_size = 16
    for prefix, type_name, count in datasets:
        value_type = type_name.encode()
        for offset in range(0, count, scan_batch_size):
            batch = [key(prefix, index)
                     for index in range(offset,
                                        min(offset + scan_batch_size, count))]
            client.connection.sendall(b"".join(
                encode_command(("TYPE", item)) for item in batch
            ))
            for item in batch:
                actual_type = client.read_response()
                if actual_type != value_type:
                    raise RuntimeError(
                        f"type mismatch for {item!r}: {actual_type!r}, "
                        f"expected {value_type!r}"
                    )

            if type_name == "zset":
                for item in batch:
                    cardinality = client.command("ZCARD", item)
                    add_field(digest, value_type)
                    add_field(digest, item)
                    add_field(digest, cardinality)
                    for start in range(0, cardinality, 1000):
                        response = client.command(
                            "ZRANGE", item, start, min(start + 999, cardinality - 1),
                            "WITHSCORES"
                        )
                        for member, score in zip(response[::2], response[1::2]):
                            add_field(digest, member)
                            add_field(digest, format(float(score), ".17g"))
                continue

            client.connection.sendall(b"".join(
                encode_command(commands[type_name](item)) for item in batch
            ))
            for item in batch:
                response = client.read_response()
                add_field(digest, value_type)
                add_field(digest, item)
                if type_name == "hash":
                    pairs = sorted(zip(response[::2], response[1::2]))
                    add_field(digest, len(pairs))
                    for field, value in pairs:
                        add_field(digest, field)
                        add_field(digest, value)
                elif type_name == "set":
                    values = sorted(response)
                    add_field(digest, len(values))
                    for value in values:
                        add_field(digest, value)
                elif type_name == "list":
                    add_field(digest, len(response))
                    for value in response:
                        add_field(digest, value)
                else:
                    add_field(digest, response)
    return digest.hexdigest()


class Profiler:
    def __init__(self, client, args):
        self.client = client
        self.args = args
        self.settle()
        baseline = metrics(client, args, "BASELINE")
        self.baseline_allocated = baseline["allocator_allocated_bytes"]
        baseline["accounting_status"] = "not_applicable"
        self.rows = [baseline]

    def settle(self):
        settle_rss(self.args.pid, self.args.settle_stable,
                   self.args.settle_max)

    def record(self, phase, verify=None):
        self.settle()
        row = metrics(
            self.client, self.args, phase, self.baseline_allocated
        )
        self.rows.append(row)
        if verify is not None:
            row["snapshot_sha256"] = snapshot(self.client, verify)
        print(f"{self.args.allocator}: {phase}: {row['keys']} keys", file=sys.stderr)

    def trim(self, phase):
        """With --trim, ask the allocator to release memory and re-measure."""
        if self.args.trim and trim_allocator(self.client, self.args):
            self.record(phase + "-trim")

    def flush(self, phase):
        self.client.command("FLUSHALL")
        self.record(phase)
        self.trim(phase)


W7_OVERWRITE_SIZES = (256, 1024, 128)


def workload_shape(args):
    """Counts for every workload.

    By default --scale shrinks per-container sizes along with key counts
    (hash fields, list elements, set members, sorted-set members), which
    changes the shape of W3-W6. With --fixed-shape only the number of
    containers is scaled and each container keeps its specified size.
    """
    scale = args.scale

    def inner(count):
        return count if args.fixed_shape else sized(count, scale)

    return {
        "keys_1m": sized(1_000_000, scale),
        "hash_count": sized(100_000, scale),
        "hash_fields": inner(10),
        "list_count": sized(100_000, scale),
        "list_members": inner(100),
        "set_count": sized(100_000, scale),
        "set_members": inner(50),
        "zset_count": sized(10, scale),
        "zset_members": inner(100_000),
    }


def run_w1(profiler, client, args, shape):
    count = shape["keys_1m"]
    fill_strings(client, count, 64, args.seed)
    profiler.record("W1-fill", verify=[("s", "string", count)])
    profiler.flush("W1-free")


def run_w2(profiler, client, args, shape):
    count = shape["keys_1m"]
    for value_size, label in ((256, "256"), (1024, "1024")):
        fill_strings(client, count, value_size, args.seed)
        profiler.record(f"W2-{label}-fill", verify=[("s", "string", count)])
        profiler.flush(f"W2-{label}-free")


def run_w3(profiler, client, args, shape):
    fill_hashes(client, shape["hash_count"], shape["hash_fields"], args.seed)
    profiler.record("W3-fill", verify=[("h", "hash", shape["hash_count"])])
    profiler.flush("W3-free")


def run_w4(profiler, client, args, shape):
    fill_collections(client, "list", shape["list_count"],
                     shape["list_members"], args.seed)
    profiler.record("W4-fill", verify=[("l", "list", shape["list_count"])])
    profiler.flush("W4-free")


def run_w5(profiler, client, args, shape):
    fill_collections(client, "set", shape["set_count"],
                     shape["set_members"], args.seed)
    profiler.record("W5-fill", verify=[("u", "set", shape["set_count"])])
    profiler.flush("W5-free")


def run_w6(profiler, client, args, shape):
    fill_zsets(client, shape["zset_count"], shape["zset_members"])
    profiler.record("W6-fill", verify=[("z", "zset", shape["zset_count"])])
    profiler.flush("W6-free")


def run_w7(profiler, client, args, shape):
    seed = args.seed
    keys_1m = shape["keys_1m"]
    fill_strings(client, keys_1m, 64, seed)
    profiler.record("W7-fill")
    live_ids = list(range(keys_1m))
    generator = random.Random(seed)
    for round_number, value_size in enumerate(W7_OVERWRITE_SIZES, start=1):
        overwritten = generator.sample(range(keys_1m), keys_1m // 2)
        fill_strings(client, keys_1m, value_size, seed, round_number, overwritten)
        profiler.record(f"W7-round{round_number}-overwrite")

        delete_count = max(1, len(live_ids) * 3 // 10)
        deleted = generator.sample(live_ids, delete_count)
        generated_pipeline(
            client, len(deleted),
            lambda position: ("DEL", key("s", deleted[position]))
        )
        deleted_set = set(deleted)
        live_ids = [index for index in live_ids if index not in deleted_set]
        profiler.record(f"W7-round{round_number}-delete")

        fill_strings(client, keys_1m, 64, seed, round_number + 10, deleted)
        live_ids.extend(deleted)
        live_ids.sort()
        profiler.record(f"W7-round{round_number}-refill",
                        verify=([("s", "string", keys_1m)]
                                if round_number == len(W7_OVERWRITE_SIZES)
                                else None))
    profiler.flush("W7-free")


def run_w8(profiler, client, args, shape):
    hash_count = shape["hash_count"]
    fill_hashes(client, hash_count, shape["hash_fields"], args.seed)
    profiler.record("W8-del-fill")
    hash_keys = [key("h", index) for index in range(hash_count)]
    run_commands(
        client,
        (("DEL", *hash_keys[offset:offset + 128])
         for offset in range(0, len(hash_keys), 128)),
    )
    profiler.record("W8-del-free")
    profiler.trim("W8-del-free")
    fill_hashes(client, hash_count, shape["hash_fields"], args.seed)
    profiler.record("W8-flush-fill")
    profiler.flush("W8-flushall-free")


WORKLOADS = {
    "W1": run_w1, "W2": run_w2, "W3": run_w3, "W4": run_w4,
    "W5": run_w5, "W6": run_w6, "W7": run_w7, "W8": run_w8,
}


def execute(client, args):
    profiler = Profiler(client, args)
    shape = workload_shape(args)
    for name in args.workloads:
        WORKLOADS[name](profiler, client, args, shape)
    return profiler.rows


def wait_for_server(host, port, kind, username, password):
    deadline = time.monotonic() + 30
    last_error = None
    while time.monotonic() < deadline:
        try:
            client = Client(host, port)
            if client.command("PING") != b"PONG":
                raise RuntimeError("server did not answer PING")
            if kind == "clone":
                if client.command("AUTH", username, password) != b"OK":
                    raise RuntimeError("clone authentication failed")
            return client
        except (ConnectionError, OSError) as error:
            last_error = error
            time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for server: {last_error}")


def parse_workloads(value, parser):
    if value.strip().upper() == "ALL":
        return list(WORKLOADS)
    selected = [item.strip().upper() for item in value.split(",")
                if item.strip()]
    unknown = [item for item in selected if item not in WORKLOADS]
    if unknown or not selected:
        parser.error("--workloads must be ALL or a comma list of "
                     + ",".join(WORKLOADS))
    return selected


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--kind", choices=("clone", "redis"), required=True)
    parser.add_argument("--allocator", required=True)
    parser.add_argument("--username", default="bench")
    parser.add_argument("--password", default="benchmark-password")
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--output", required=True)
    parser.add_argument("--workloads", default="ALL",
                        help="ALL or a comma list such as W1,W2")
    parser.add_argument("--fixed-shape", action="store_true",
                        help="scale only container counts; keep per-container "
                             "sizes (fields, elements, members) at spec")
    parser.add_argument("--trim", action="store_true",
                        help="after each free phase ask the allocator to "
                             "release memory and record an extra -trim row")
    parser.add_argument("--settle-max", type=float, default=12.0,
                        help="max seconds to wait for RSS to stop moving "
                             "before each measurement (0 disables)")
    parser.add_argument("--settle-stable", type=float, default=1.0,
                        help="seconds RSS must stay within 256 KiB to count "
                             "as settled")
    args = parser.parse_args()
    if args.port < 1 or args.port > 65535:
        parser.error("--port must be from 1 to 65535")
    if args.pid <= 0 or not 0 < args.scale <= 1:
        parser.error("--pid must be positive and --scale must be in (0, 1]")
    if args.settle_max < 0 or args.settle_stable <= 0:
        parser.error("--settle-max must be >= 0 and --settle-stable > 0")
    args.workloads = parse_workloads(args.workloads, parser)

    client = wait_for_server(args.host, args.port, args.kind,
                             args.username, args.password)
    try:
        rows = execute(client, args)
    finally:
        client.close()

    fields = ("target", "workload", "phase", "allocator", "keys",
              "used_bytes", "allocator_allocated_bytes",
              "allocator_resident_bytes", "process_rss_bytes",
              "fragmentation_ratio", "rss_over_allocated", "bytes_per_key",
              "accounting_baseline_bytes", "accounting_delta_bytes",
              "accounting_tolerance_bytes", "accounting_status",
              "snapshot_sha256")
    with open(args.output, "w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"mem_profile: {error}", file=sys.stderr)
        raise