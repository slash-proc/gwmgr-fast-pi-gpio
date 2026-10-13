"""Fallback implementation for the optional Cython support module.

The native probe itself is the C executable.  These helpers are deliberately
small: they keep process/socket supervision replaceable without making Cython
a runtime requirement for GnWmanager.
"""

from __future__ import annotations

import os
import socket


def pid_alive(pid: int) -> bool:
    """Return whether *pid* still exists for this user."""

    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def tcp_ready(host: str, port: int, timeout: float = 0.2) -> bool:
    """Return whether a TCP listener accepts a connection."""

    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False
