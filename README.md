# GnWManager fast Pi GPIO backend

This repository contains a native Raspberry Pi SWD backend for
[GnWManager](https://github.com/slash-proc/gnwmanager). It is intended for
running GnWManager against an STM32 target through the Raspberry Pi's GPIO
pins, without OpenOCD in the Pi-side data path.

The maintained path is:

```text
GnWManager or web-builder
        │
        │  GnWManager's normal GDB backend
        ▼
GDB remote protocol over TCP
        ▼
gdb-swd (this repository, native C)
        ▼
GPIO MMIO and SWD
        ▼
STM32 target
```

The current implementation has been used against real hardware for repeated
recovery, memory access, and flash operations. The native server is the part
to use for that workflow. The CMSIS-DAP, DMA, SPI, and FunctionFS programs
remain useful experiments and diagnostics, but are not required by the
GnWManager integration.

## What is included

### `gdb-swd`

`gdb-swd` is a small GDB Remote Serial Protocol server. It implements the
operations used by GnWManager's GDB backend and translates them directly to
the GPIO/SWD engine:

- Cortex-M register reads and writes
- memory reads and writes
- halt, resume, reset, and interrupt handling
- GDB target-description XML, including Cortex-M system registers
- external-flash address access used by the recovery workflow
- sequential memory-read batching and GDB binary memory reads

It is intentionally a narrow GDB server rather than a complete debugger
stub. Its protocol behavior is compatible with ordinary GDB clients for the
implemented operations, but the primary client is GnWManager.

### `gnwmanager-fastpi`

The Python package is an optional supervisor. It starts `gdb-swd`, waits for
the TCP listener, launches an otherwise unchanged GnWManager process with the
GDB backend options inserted, and shuts both processes down together.

The hardware path is native C. Cython is optional and only replaces a small
process/socket-supervision boundary; it is not needed to use the probe.

## Requirements

### Raspberry Pi

- Raspberry Pi 4 or a compatible Linux Raspberry Pi system
- 64-bit ARM userspace is recommended
- Linux GPIO memory access through `/dev/gpiomem` or equivalent permission
- CMake 3.16 or newer
- a C11 compiler and standard build tools
- Python 3.10 or newer if the supervisor package is used
- GnWManager installed and available as `gnwmanager`

The default build is tuned for the Raspberry Pi 4 Cortex-A72 when built on an
ARM64 system. The probe does not change the CPU governor automatically.

### Target wiring

The default pins are:

| Signal | Raspberry Pi GPIO | Physical pin |
| --- | ---: | ---: |
| SWCLK | GPIO 11 | 23 |
| SWDIO | GPIO 10 | 19 |
| GND | GND | any ground pin |

Connect a common ground. The target and Pi must use compatible 3.3 V logic.
Do not connect a Pi GPIO to a 5 V signal and do not use the Pi to power an
unknown target. SWDIO is released during target-output phases by the GPIO
backend; do not add a second driver to that line.

The pins and requested clock can be changed with `gdb-swd` options. If the
target board has a reset or power-control circuit, operate it so that reset
and power transitions cannot occur while an SWD transaction is in progress.

## Build the native backend

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The unit tests exercise the GDB packet layer, SWD protocol logic, and
CMSIS-DAP command processor. They do not require a target connected to the
Pi. Hardware validation is separate and must be run on the Pi with the target
wired.

The main executable is:

```text
build/gdb-swd
```

Useful supporting executables include `swd-read`, `cmsis-dap-read`, and
`cmsis-dap-tcp`. The other binaries in `build/` are development or transport
experiments; see the source and scripts before using them on hardware.

## Run it directly

Start the native GDB server on the Pi:

```sh
./build/gdb-swd \
  --bind 127.0.0.1 \
  --port 3333 \
  --swclk 11 \
  --swdio 10 \
  --clock 128000000
```

Then point GnWManager at it from another terminal or host:

```sh
gnwmanager \
  --backend gdb \
  --gdb-host 127.0.0.1 \
  --gdb-port 3333 \
  info
```

If GnWManager is running on another machine, bind the server to an address
reachable from that machine and restrict access with the network configuration
appropriate for the setup. The GDB server does not provide authentication.

For a quick protocol check against a running server:

```sh
python3 scripts/gdb-swd-smoke.py 127.0.0.1 3333
```

The smoke test reads target memory, checks the GDB stop/interrupt behavior,
and detaches. It requires a live target and a server with working SWD access.

## Run GnWManager through the Python supervisor

Install the package from the repository root:

```sh
python3 -m pip install --no-build-isolation .
```

The optional Cython build can be requested when Cython is already available:

```sh
python3 -m pip install --no-build-isolation '.[cython]'
```

Run GnWManager by putting `--` before its arguments:

```sh
gnwmanager-fastpi -- info
```

For example, a flash operation can be run as:

```sh
gnwmanager-fastpi -- \
  flash-patch zelda \
  internal_flash_backup_zelda.bin \
  flash_backup_zelda.bin
```

The supervisor starts `gdb-swd` on `127.0.0.1:3333`, waits until the listener
is ready, and adds these GnWManager options unless they were already supplied:

```text
--backend gdb
--gdb-host 127.0.0.1
--gdb-port 3333
```

Supervisor options:

```text
--probe-binary PATH   use a specific gdb-swd binary
--probe-bind ADDRESS  probe listen address (default: 127.0.0.1)
--probe-port PORT     probe listen port (default: 3333)
--swclk PIN           SWCLK GPIO (default: 11)
--swdio PIN           SWDIO GPIO (default: 10)
--clock HZ            requested SWCLK (default: 128000000)
--gnwmanager PATH     GnWManager executable
```

The binary is located in this order:

1. `GWMGR_FAST_PI_GPIO_GDB_SWD`, if set;
2. `gdb-swd` on `PATH`;
3. `build/gdb-swd` in the current checkout;
4. `build/gdb-swd` next to the installed package checkout.

For example:

```sh
GWMGR_FAST_PI_GPIO_GDB_SWD=/opt/gwn/gdb-swd \
  gnwmanager-fastpi --probe-port 3334 -- info
```

The supervisor does not replace GnWManager's GDB implementation. It only
owns the native probe process and supplies the connection parameters.

## Clock and reliability

`--clock` is the requested SWCLK rate used by the GPIO timing engine. At the
highest settings it is a throughput setting, not a precision frequency
reference. Actual timing depends on the Pi model, CPU frequency, compiler
options, kernel scheduling, and GPIO access latency.

The default native GDB-server setting is 128 MHz because that is the fastest
setting validated for the current hardware path. Use a lower value first when
bringing up a new target or wiring arrangement. A logic analyser is the right
tool for validating signal quality and edge timing.

Do not run multiple GDB clients against one target. In particular, do not
leave a second GDB server, OpenOCD instance, or independent probe session
connected to the same SWD pins while GnWManager is operating.

When a target loses debug access during reset or flash operations, the correct
recovery is normally to let the current client reconnect and reinitialize SWD,
not to issue unrelated memory requests through the stale session. The native
server keeps the TCP session alive for the halt-failure sequence expected by
GnWManager, while a genuinely failed session is reinitialized on reconnect.

## Useful hardware checks

The direct SWD reader can validate the electrical path before involving GDB:

```sh
./build/swd-read \
  --address 0x24000000 \
  --length 262144 \
  --output ram.bin
```

The fused GPIO reader is available with:

```sh
./build/swd-read \
  --fast-gpio \
  --clock 128000000 \
  --address 0x24000000 \
  --length 262144 \
  --output ram.bin
```

The STM32H7B0 AXI SRAM address above is an example for the target used during
development. Use an address and length that are valid for the actual target;
the length must be divisible by four.

For a repeatable local benchmark on the Pi:

```sh
bash scripts/benchmark-fastest.sh
```

The script temporarily selects the `performance` CPU governor when possible
and restores the original setting on exit. It rejects a run if the reference
memory hash does not match.

## CMSIS-DAP and other transports

The repository also contains:

- a unit-tested CMSIS-DAP packet processor;
- a GPIO-backed CMSIS-DAP implementation;
- a CMSIS-DAP-over-TCP server;
- DMA and SPI timing experiments;
- an OpenOCD GPIO interface configuration;
- historical USB FunctionFS experiments.

These components are not required for the maintained GnWManager path. The
native GDB server is preferred because it removes an extra OpenOCD process and
matches the GDB operations GnWManager already uses.

The CMSIS-DAP TCP configuration is retained for clients or OpenOCD builds that
support that transport, but the OpenOCD version available during development
did not accept the required TCP backend. Treat that path as experimental until
the client side has been verified independently.

## Repository layout

```text
src/                 native SWD, GPIO, GDB, and transport implementations
include/             public C headers
tests/               host-side protocol tests
scripts/             hardware smoke tests, benchmarks, and experiments
openocd/              OpenOCD interface configurations
python/               optional gnwmanager-fastpi supervisor package
docs/                hardware notes and KiCad relay/power-control files
```

## Project status

The supported and maintained integration is the direct native GDB/SWD path:

```text
gdb-swd + GnWManager GDB backend + Raspberry Pi GPIO + STM32 SWD
```

The Python supervisor is convenience tooling around that path. It is small,
has no runtime dependencies beyond Python, and is not required when the
processes are managed separately.

The DMA and SPI work is intentionally kept in the repository as measured
experiments rather than presented as the default backend. Any change to the
default clock, GPIO timing, reset behavior, or GDB packet handling should be
validated both with the host-side tests and on real hardware.
