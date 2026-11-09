#!/usr/bin/env python3
"""Compare randomized sorted-set commands with a local Redis server."""

import argparse
import os
import random
import socket
import subprocess
import tempfile
import time


def available_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def read_exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise RuntimeError("unexpected EOF reading RESP response")
    return data


def read_line(stream):
    line = stream.readline()
    if not line.endswith(b"\r\n"):
        raise RuntimeError("invalid RESP line")
    return line[:-2]


def read_response(stream):
    marker = read_exact(stream, 1)
    if marker == b"+":
        return read_line(stream)
    if marker == b"-":
        raise RuntimeError("server returned RESP error: " + read_line(stream).decode())
    if marker == b":":
        return int(read_line(stream))
    if marker == b"$":
        length = int(read_line(stream))
        if length == -1:
            return None
        value = read_exact(stream, length)
        if read_exact(stream, 2) != b"\r\n":
            raise RuntimeError("invalid RESP bulk string terminator")
        return value
    if marker == b"*":
        length = int(read_line(stream))
        if length == -1:
            return None
        return [read_response(stream) for _ in range(length)]
    raise RuntimeError("unknown RESP response marker: " + repr(marker))


class Client:
    def __init__(self, port):
        self.connection = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.connection.settimeout(5)
        self.stream = self.connection.makefile("rb")

    def command(self, *arguments):
        encoded = [str(argument).encode() for argument in arguments]
        request = [b"*" + str(len(encoded)).encode() + b"\r\n"]
        for value in encoded:
            request.extend(
                (b"$" + str(len(value)).encode() + b"\r\n", value, b"\r\n")
            )
        self.connection.sendall(b"".join(request))
        return read_response(self.stream)

    def close(self):
        self.stream.close()
        self.connection.close()


def wait_for_server(process, port, authentication=None):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("server exited before accepting connections")
        try:
            client = Client(port)
            if client.command("PING") != b"PONG":
                raise RuntimeError("server did not answer PING")
            if authentication is not None:
                if client.command("AUTH", *authentication) != b"OK":
                    raise RuntimeError("clone server authentication failed")
            return client
        except (ConnectionError, OSError):
            time.sleep(0.05)
    raise RuntimeError("timed out waiting for server on port " + str(port))


def start_server(command, port):
    return subprocess.Popen(
        command(port),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def stop_server(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def run_differential_test(clone_binary, redis_binary, operations, seed):
    clone_port = available_port()
    redis_port = available_port()
    clone = None
    reference = None
    clone_process = None
    redis_process = None
    try:
        with tempfile.TemporaryDirectory(prefix="redis-clone-differential-") as directory:
            users_file = os.path.join(directory, "users.conf")
            with open(users_file, "w", encoding="utf-8") as users:
                users.write("differential-test secret-password\n")
            clone_process = start_server(
                lambda port: [clone_binary, str(port), users_file], clone_port
            )
            redis_process = start_server(
                lambda port: [
                    redis_binary,
                    "--bind",
                    "127.0.0.1",
                    "--port",
                    str(port),
                    "--protected-mode",
                    "no",
                    "--save",
                    "",
                    "--appendonly",
                    "no",
                    "--loglevel",
                    "warning",
                ],
                redis_port,
            )
            clone = wait_for_server(
                clone_process,
                clone_port,
                ("differential-test", "secret-password"),
            )
            reference = wait_for_server(redis_process, redis_port)
        generator = random.Random(seed)
        key = "differential-zset"
        for index in range(operations):
            member = "member-" + str(generator.randrange(80))
            choice = generator.randrange(8)
            if choice <= 1:
                command = ("ZADD", key, generator.randrange(-15, 16), member)
            elif choice == 2:
                command = ("ZREM", key, member)
            elif choice == 3:
                command = ("ZSCORE", key, member)
            elif choice == 4:
                command = ("ZRANK", key, member)
            elif choice == 5:
                command = ("ZREVRANK", key, member)
            elif choice == 6:
                command = ("ZCARD", key)
            else:
                command = ("ZRANGEBYSCORE", key, -8, 8, "WITHSCORES")

            actual = clone.command(*command)
            expected = reference.command(*command)
            if actual != expected:
                raise AssertionError(
                    "command mismatch at operation "
                    + str(index)
                    + ": "
                    + repr(command)
                    + "; clone="
                    + repr(actual)
                    + ", Redis="
                    + repr(expected)
                )

            snapshot_command = ("ZRANGE", key, 0, -1, "WITHSCORES")
            actual_snapshot = clone.command(*snapshot_command)
            expected_snapshot = reference.command(*snapshot_command)
            if actual_snapshot != expected_snapshot:
                raise AssertionError(
                    "sorted-set contents differ after operation "
                    + str(index)
                    + ": clone="
                    + repr(actual_snapshot)
                    + ", Redis="
                    + repr(expected_snapshot)
                )
    finally:
        if clone is not None:
            clone.close()
        if reference is not None:
            reference.close()
        if clone_process is not None:
            stop_server(clone_process)
        if redis_process is not None:
            stop_server(redis_process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clone", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--operations", type=int, default=500)
    parser.add_argument("--seed", type=int, default=20261109)
    args = parser.parse_args()
    if args.operations <= 0:
        parser.error("--operations must be positive")
    run_differential_test(
        args.clone, args.reference, args.operations, args.seed
    )


if __name__ == "__main__":
    main()
