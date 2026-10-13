"""Optional Cython implementation of the tiny supervision boundary.

This is intentionally not in the SWD hot path.  The timing-sensitive GPIO
and GDB protocol code stays in the existing C executable; Cython only avoids
duplicating process/socket plumbing if a compiled Python package is desired.
"""

from libc.sys.types cimport pid_t
from libc.unistd cimport kill

import socket


cpdef bint pid_alive(int pid):
    cdef int result
    if pid <= 0:
        return False
    result = kill(<pid_t>pid, 0)
    return result == 0


cpdef bint tcp_ready(str host, int port, double timeout=0.2):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False
