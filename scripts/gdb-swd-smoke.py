#!/usr/bin/env python3
"""Small on-target smoke test for the native GDB/SWD server."""

from __future__ import annotations

import hashlib
import socket
import sys


def checksum(payload: bytes) -> bytes:
    return f"{sum(payload) & 0xff:02x}".encode("ascii")


def recv_exact(sock: socket.socket, count: int) -> bytes:
    result = bytearray()
    while len(result) < count:
        chunk = sock.recv(count - len(result))
        if not chunk:
            raise RuntimeError("GDB server closed the connection")
        result.extend(chunk)
    return bytes(result)


def recv_packet(sock: socket.socket) -> bytes:
    while recv_exact(sock, 1) != b"$":
        pass
    payload = bytearray()
    escaped = False
    while True:
        value = recv_exact(sock, 1)
        if value == b"#":
            break
        if escaped:
            payload.append(value[0] ^ 0x20)
            escaped = False
        elif value == b"}":
            escaped = True
        else:
            payload.extend(value)
    received = recv_exact(sock, 2)
    if received.lower() != checksum(bytes(payload)):
        raise RuntimeError("bad checksum from GDB server")
    return bytes(payload)


def exchange(sock: socket.socket, payload: bytes) -> bytes:
    sock.sendall(b"$" + payload + b"#" + checksum(payload))
    if recv_exact(sock, 1) != b"+":
        raise RuntimeError("GDB server did not acknowledge the request")
    return recv_packet(sock)


def continue_target(sock: socket.socket) -> None:
    payload = b"c"
    sock.sendall(b"$" + payload + b"#" + checksum(payload))
    if recv_exact(sock, 1) != b"+":
        raise RuntimeError("GDB server did not acknowledge continue")


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 3333
    with socket.create_connection((host, port), timeout=10.0) as sock:
        sock.settimeout(10.0)
        supported = exchange(sock, b"qSupported")
        if not supported.startswith(b"PacketSize="):
            raise RuntimeError(f"unexpected qSupported response: {supported!r}")

        stop = exchange(sock, b"?")
        if stop not in (b"S02", b"S05"):
            raise RuntimeError(f"unexpected stop response: {stop!r}")

        data = exchange(sock, b"m24000000,10000")
        if len(data) != 0x10000 * 2:
            raise RuntimeError(f"unexpected memory response length: {len(data)}")
        memory = bytes.fromhex(data.decode("ascii"))
        digest = hashlib.sha256(memory).hexdigest()
        print(f"qSupported={supported.decode()} stop={stop.decode()} "
              f"read={len(memory)} hash={digest}")

        continue_target(sock)
        sock.sendall(b"\x03")
        if recv_packet(sock) != b"S02":
            raise RuntimeError("interrupt did not stop the target")
        sock.sendall(b"+")
        if exchange(sock, b"?") != b"S02":
            raise RuntimeError("target did not report halted after interrupt")

        if exchange(sock, b"D") != b"OK":
            raise RuntimeError("detach failed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"gdb-swd smoke failed: {error}", file=sys.stderr)
        raise SystemExit(1)
