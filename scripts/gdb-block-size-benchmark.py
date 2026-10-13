#!/usr/bin/env python3
"""Benchmark native GDB/SWD reads with different request sizes."""

from __future__ import annotations

import hashlib
import socket
import sys
import time


ADDRESS = 0x24000000
LENGTH = 256 * 1024
EXPECTED = "0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884"


def checksum(payload: bytes) -> bytes:
    return f"{sum(payload) & 0xff:02x}".encode("ascii")


class PacketReader:
    """Buffered reader for the byte-oriented GDB framing parser."""

    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buffer = bytearray()

    def recv_exact(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.sock.recv(max(65536, count - len(self.buffer)))
            if not chunk:
                raise RuntimeError("GDB server closed the connection")
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:count])
        del self.buffer[:count]
        return result

    def recv_until(self, delimiter: int) -> bytes:
        while True:
            try:
                index = self.buffer.index(delimiter)
            except ValueError:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("GDB server closed the connection")
                self.buffer.extend(chunk)
                continue
            result = bytes(self.buffer[:index])
            del self.buffer[: index + 1]
            return result


def recv_exact(reader: PacketReader, count: int) -> bytes:
    return reader.recv_exact(count)


def recv_packet(reader: PacketReader) -> bytes:
    reader.recv_until(ord("$"))
    wire_payload = reader.recv_until(ord("#"))
    if b"}" not in wire_payload:
        payload = wire_payload
    else:
        chunks = wire_payload.split(b"}")
        payload_buffer = bytearray(chunks[0])
        for chunk in chunks[1:]:
            if not chunk:
                raise RuntimeError("unterminated GDB escape sequence")
            payload_buffer.append(chunk[0] ^ 0x20)
            payload_buffer.extend(chunk[1:])
        payload = bytes(payload_buffer)
    if recv_exact(reader, 2).lower() != checksum(payload):
        raise RuntimeError("bad GDB response checksum")
    return payload


def exchange(sock: socket.socket, reader: PacketReader, payload: bytes) -> bytes:
    sock.sendall(b"$" + payload + b"#" + checksum(payload))
    if recv_exact(reader, 1) != b"+":
        raise RuntimeError("GDB server did not acknowledge the request")
    return recv_packet(reader)


def halt_target(sock: socket.socket, reader: PacketReader) -> None:
    sock.sendall(b"\x03")
    if recv_packet(reader) != b"S02":
        raise RuntimeError("target did not halt")
    sock.sendall(b"+")


def read_once(
    host: str, port: int, block_size: int, binary_mode: bool
) -> tuple[float, str]:
    data = bytearray()
    with socket.create_connection((host, port), timeout=20.0) as sock:
        sock.settimeout(20.0)
        reader = PacketReader(sock)
        exchange(sock, reader, b"qSupported")
        halt_target(sock, reader)
        start = time.perf_counter()
        for offset in range(0, LENGTH, block_size):
            count = min(block_size, LENGTH - offset)
            command_name = "x" if binary_mode else "m"
            command = f"{command_name}{ADDRESS + offset:x},{count:x}".encode(
                "ascii"
            )
            reply = exchange(sock, reader, command)
            chunk = reply if binary_mode else bytes.fromhex(reply.decode("ascii"))
            if len(chunk) != count:
                raise RuntimeError(
                    f"short read for {block_size}: requested {count}, got {len(chunk)}"
                )
            data.extend(chunk)
        elapsed = time.perf_counter() - start
        if exchange(sock, reader, b"D") != b"OK":
            raise RuntimeError("detach failed")
    digest = hashlib.sha256(data).hexdigest()
    prefix = "gdb-binary" if binary_mode else "gdb"
    with open(f"/tmp/{prefix}-{block_size}.bin", "wb") as output:
        output.write(data)
    return elapsed, digest


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 3333
    binary_mode = "--binary" in sys.argv[3:]
    print("block_size,elapsed_s,rate_kib_s,sha256")
    for block_size in (4096, 16384, 65536):
        elapsed, digest = read_once(host, port, block_size, binary_mode)
        rate = LENGTH / 1024.0 / elapsed
        status = "ok" if digest == EXPECTED else "mismatch"
        print(f"{block_size},{elapsed:.6f},{rate:.1f},{status},{digest}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"GDB block-size benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(1)
