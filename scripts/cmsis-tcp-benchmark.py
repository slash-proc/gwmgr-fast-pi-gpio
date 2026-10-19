#!/usr/bin/env python3
"""Benchmark the gwmgr-fast-pi-gpio CMSIS-DAP TCP framing with TransferBlock."""

from __future__ import annotations

import argparse
import hashlib
import socket
import struct
import time


EXPECTED = "0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884"
SIGNATURE = 0x00504144
HEADER = struct.Struct("<I H B B")


def receive_exact(sock: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise RuntimeError("CMSIS-DAP TCP connection closed")
        data.extend(chunk)
    return bytes(data)


def command(sock: socket.socket, payload: bytes) -> bytes:
    sock.sendall(HEADER.pack(SIGNATURE, len(payload), 1, 0) + payload)
    header = receive_exact(sock, HEADER.size)
    signature, length, packet_type, reserved = HEADER.unpack(header)
    if (signature, packet_type, reserved) != (SIGNATURE, 2, 0):
        raise RuntimeError(f"invalid response header: {header.hex()}")
    return receive_exact(sock, length)


def transfer_write(sock: socket.socket, request: int, value: int) -> None:
    response = command(sock, struct.pack("<BBBBI", 0x05, 0, 1, request, value))
    if response != bytes((0x05, 1, 1)):
        raise RuntimeError(f"CMSIS-DAP write failed: {response.hex()}")


def transfer_read(sock: socket.socket, request: int) -> int:
    response = command(sock, bytes((0x05, 0, 1, request)))
    if len(response) != 7 or response[:3] != bytes((0x05, 1, 1)):
        raise RuntimeError(f"CMSIS-DAP read failed: {response.hex()}")
    return struct.unpack_from("<I", response, 3)[0]


def prepare(sock: socket.socket, address: int) -> None:
    if command(sock, bytes((0x02, 1))) != bytes((0x02, 1)):
        raise RuntimeError("DAP_Connect failed")
    transfer_write(sock, 0x00, 0x0000001E)
    transfer_write(sock, 0x04, 0x50000000)
    for _ in range(100):
        if transfer_read(sock, 0x06) & 0xA0000000 == 0xA0000000:
            break
    else:
        raise RuntimeError("target did not power up")
    transfer_write(sock, 0x08, 0)
    transfer_write(sock, 0x01, 0x23000052)
    transfer_write(sock, 0x05, address)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4441)
    parser.add_argument("--packet-size", type=int, default=4096)
    parser.add_argument("--length", type=int, default=262144)
    args = parser.parse_args()
    if args.length % 4 or args.packet_size < 8:
        parser.error("length must be divisible by four and packet size must be at least eight")

    words_per_packet = (args.packet_size - 4) // 4
    memory = bytearray()
    with socket.create_connection((args.host, args.port)) as sock:
        prepare(sock, 0x24000000)
        start = time.monotonic()
        remaining = args.length // 4
        while remaining:
            count = min(remaining, words_per_packet)
            response = command(sock, bytes((0x06, 0)) + struct.pack("<H", count) + b"\x0f")
            expected_length = 4 + count * 4
            if (len(response) != expected_length or response[:4] !=
                    bytes((0x06, count & 0xFF, count >> 8, 1))):
                raise RuntimeError(f"TransferBlock failed: {response[:8].hex()}")
            memory.extend(response[4:])
            remaining -= count
        elapsed = time.monotonic() - start
        command(sock, b"\x03")

    digest = hashlib.sha256(memory).hexdigest()
    if digest != EXPECTED:
        raise RuntimeError(f"hash mismatch: {digest}")
    print(f"bytes={len(memory)} elapsed_s={elapsed:.6f} "
          f"rate_kib_s={len(memory) / 1024 / elapsed:.1f} sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
