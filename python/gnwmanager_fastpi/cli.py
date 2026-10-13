"""Supervise the native GDB/SWD server and an unchanged GnWManager process."""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

from ._native import tcp_ready


def _default_probe() -> str:
    configured = os.environ.get("RPI_FAST_GPIO_GDB_SWD")
    if configured:
        return configured
    found = shutil.which("gdb-swd")
    if found:
        return found
    candidates = (
        Path.cwd() / "build" / "gdb-swd",
        Path(__file__).resolve().parents[2] / "build" / "gdb-swd",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return "gdb-swd"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gnwmanager-fastpi",
        description="Run GnWManager through the native Pi GDB/SWD backend.",
        allow_abbrev=False,
    )
    parser.add_argument("--probe-binary", default=_default_probe())
    parser.add_argument("--probe-bind", default="127.0.0.1")
    parser.add_argument("--probe-port", type=int, default=3333)
    parser.add_argument("--swclk", type=int, default=11)
    parser.add_argument("--swdio", type=int, default=10)
    parser.add_argument("--clock", type=int, default=128_000_000)
    parser.add_argument("--gnwmanager", default=os.environ.get("GNWMANAGER", "gnwmanager"))
    parser.add_argument(
        "gnwmanager_args",
        nargs=argparse.REMAINDER,
        help="arguments passed unchanged to GnWManager (put -- before them)",
    )
    return parser


def _with_gdb_backend(arguments: list[str], host: str, port: int) -> list[str]:
    """Add the GDB backend options without modifying GnWManager itself."""

    result = list(arguments)
    pairs = {
        "--backend": "gdb",
        "--gdb-host": host,
        "--gdb-port": str(port),
    }
    for option, expected in pairs.items():
        values: list[str] = []
        for index, argument in enumerate(result):
            if argument == option and index + 1 < len(result):
                values.append(result[index + 1])
            elif argument.startswith(option + "="):
                values.append(argument.split("=", 1)[1])
        if values and any(value != expected for value in values):
            raise ValueError(f"GnWManager argument {option} conflicts with the native probe")
        if not values:
            result[0:0] = [option, expected]
    return result


def _strip_remainder_separator(arguments: list[str]) -> list[str]:
    return arguments[1:] if arguments[:1] == ["--"] else list(arguments)


def _wait_for_port(process: subprocess.Popen[bytes], host: str, port: int) -> None:
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"gdb-swd exited with status {process.returncode}")
        if tcp_ready(host, port):
            return
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for gdb-swd at {host}:{port}")


def _terminate(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    options = parser.parse_args(argv)
    options.gnwmanager_args = _strip_remainder_separator(options.gnwmanager_args)
    try:
        gnwmanager_args = _with_gdb_backend(
            options.gnwmanager_args, options.probe_bind, options.probe_port
        )
    except ValueError as error:
        parser.error(str(error))

    probe_command = [
        options.probe_binary,
        "--bind",
        options.probe_bind,
        "--port",
        str(options.probe_port),
        "--swclk",
        str(options.swclk),
        "--swdio",
        str(options.swdio),
        "--clock",
        str(options.clock),
    ]
    probe: subprocess.Popen[bytes] | None = None
    manager: subprocess.Popen[bytes] | None = None
    stopping = False

    def stop_children(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True
        _terminate(manager)
        _terminate(probe)

    signal.signal(signal.SIGINT, stop_children)
    signal.signal(signal.SIGTERM, stop_children)
    try:
        probe = subprocess.Popen(probe_command)
        _wait_for_port(probe, options.probe_bind, options.probe_port)
        manager = subprocess.Popen([options.gnwmanager, *gnwmanager_args])
        while manager.poll() is None:
            if probe.poll() is not None:
                raise RuntimeError(f"gdb-swd exited with status {probe.returncode}")
            time.sleep(0.1)
        return manager.returncode or 0
    except (OSError, RuntimeError, TimeoutError) as error:
        if not stopping:
            print(f"gnwmanager-fastpi: {error}", file=sys.stderr)
        return 1
    finally:
        _terminate(manager)
        _terminate(probe)
