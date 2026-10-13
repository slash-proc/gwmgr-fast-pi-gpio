# rpi-fast-gpio

An experimental Raspberry Pi 4 SWD probe. The long-term target is a CMSIS-DAPv2 probe with batched SWD operations and a local TCP transport for tools running on the Pi.

## Current milestone

The supported integration path is now a normal Linux process on the Pi:

```text
GnWmanager  ->  GDB TCP  ->  gdb-swd (native C)  ->  GPIO/SWD  ->  STM32H7B0
```

GnWmanager itself remains unchanged.  The `gnwmanager-fastpi` package is a
small supervisor that starts the native server, waits for its TCP listener,
injects the GDB backend options, and stops the server with GnWmanager.  Cython
is optional and only replaces that small supervision boundary; the hardware
path stays in C.

Install the supervisor on the Pi from this repository with:

```sh
python3 -m pip install --no-build-isolation .
```

Then run GnWmanager through the native probe, for example:

```sh
gnwmanager-fastpi -- flash-patch zelda internal_flash_backup_zelda.bin flash_backup_zelda.bin
```

The `--` separates supervisor options from GnWmanager arguments.  The native
server can also be started directly when GnWmanager is run separately:

```sh
sudo ./build/gdb-swd --bind 127.0.0.1 --port 3333 --clock 128000000
gnwmanager -b gdb --gdb-host 127.0.0.1 --gdb-port 3333 info
```

The dual-boot bootloader image published by the upstream repository is too
large for the remaining space at `0x08032000` on this STM32H7B0 layout.  A
size-checked optimized local build is available through
`scripts/build-gnwmanager-bootloader.sh`; it installs only with an explicit
`--install` and preserves the existing cache entry as `.release`.

The first implementation is deliberately below the USB and CMSIS-DAP layers. `swd-read` uses the Pi's GPIO MMIO interface and a CPU-driven SWD engine to:

1. switch a target into SWD mode;
2. read and print the DP IDCODE;
3. power up the debug and system domains;
4. configure the MEM-AP for 32-bit incrementing accesses; and
5. read a contiguous target RAM region, using posted/pipelined AP reads.

This is the baseline needed before deciding where DMA actually improves the design.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The primary target for this project is the STM32H7B0 device already used with `gnwmanager`. Run the hardware test on the Pi as root, or with a user that can access `/dev/gpiomem`:

```sh
sudo ./build/swd-read --address 0x24000000 --length 262144 --output ram.bin
```

For the highest-throughput CPU path currently validated on the Pi, add
`--fast-gpio`. This uses a fused GPIO/SWD memory loop rather than the generic
function-pointer interface:

```sh
sudo ./build/swd-read --fast-gpio --clock 128000000 \
  --address 0x24000000 --length 262144 --output ram.bin
```

ARM builds are tuned for the Pi 4's Cortex-A72 when
`RPI_FAST_GPIO_A72_TUNING` is enabled (the default). Keeping the CPU governor
at `performance` also avoids frequency ramp-up during short benchmark runs;
the project does not change that governor automatically.

For a reproducible three-run full-read benchmark on the Pi, use:

```sh
bash scripts/benchmark-fastest.sh
```

The script temporarily selects the `performance` governor by default, restores
the original governor on exit, and rejects a run unless the 256 KiB reference
hash matches.

Defaults are SWCLK GPIO 11, SWDIO GPIO 10, a conservative 1 MHz SWCLK, and the STM32H7B0 AXI SRAM base at `0x24000000`. Override the pins, clock, and address for the target board. The address must point at readable target RAM and the length must be divisible by four. For the STM32H7B0, `0x24000000` is preferable to the 128 KiB DTCM at `0x20000000` for this first 256 KiB test.

The matching OpenOCD interface configuration is `openocd/rpi-fast-swd.cfg`. It can be tested independently with:

```sh
openocd -f openocd/rpi-fast-swd.cfg -f target/stm32h7x.cfg -c "init; shutdown"
```

## Wiring and safety

Connect SWCLK, SWDIO, and GND. Both boards must use compatible 3.3 V logic. Do not connect the Pi GPIOs to a 5 V target and do not use the Pi to power an unknown target. SWDIO is actively driven only during host-output phases and is released for target-output phases.

## Roadmap

- [x] CPU-driven GPIO baseline and SWD DP/AP read path
- [x] Capture the Pi CPU-driven baseline over 256 KiB and sweep requested SWCLK settings
- [x] Establish a root-only DMA GPIO waveform benchmark with Pi VCSM-CMA buffers
- [x] Prove a DMA-generated SWD transaction with GPIO10 input capture and ACK/parity validation
- [x] Prove a batched 256-word DMA MEM-AP read with matching output data
- [ ] Add a GPIO/DMA waveform backend behind the same SWD transaction interface
- [ ] Add batched transfer descriptions to reduce DMA/CPU handoffs
- [x] Add a unit-tested CMSIS-DAPv2 command processor and GPIO backend for `DAP_Transfer`/`DAP_TransferBlock`
- [x] Add a local CMSIS-DAP-over-TCP server and native client benchmark
- [x] Add a native direct GDB server for the GnWManager GDB backend
- [x] Reuse sequential MEM-AP TAR state across native GDB memory requests
- [ ] Integrate the fast backend with GnWManager's local OpenOCD path
- [ ] Optionally compare against other probes after the Pi implementation is competitive

The implementation currently uses `/dev/gpiomem`, so it is intentionally a Linux baseline rather than a bare-metal probe. The DMA backend should be benchmarked against this implementation before it becomes the default.

## Latest architecture checks

The standalone `packed-read` experiment tests ADIv5 MEM-AP packed transfers
(`CSW.AddrInc=packed`) for byte, halfword, and word sizes. All three modes
read the complete 256 KiB region with the reference hash, but measure
787.9--792.7 KiB/s versus the 797--799 KiB/s fused CPU baseline. A normal
32-bit DRW read already returns four bytes per SWD response, so packed
transfers do not reduce the transaction count for this word-aligned workload.

The five-channel flat-stream DMA experiment and the SPI0 3-wire path were also
retested as consolidated sweeps. DMA did not preserve SWD phase alignment even
at low pacing rates; SPI 3-wire returned an invalid ACK (`0x0`) from 1 MHz
upward. They remain diagnostic experiments, not selected backends.

## CMSIS-DAP packet path

`cmsis_dap` implements the packet processor, and `gpio_dap` maps CMSIS-DAP
SWD requests onto the GPIO backend. An AP TAR write followed by an AP DRW
`DAP_TransferBlock` read uses the fused CPU reader, so the USB packet layer
does not reintroduce one host-side operation per target word. The hardware
exerciser is:

```sh
sudo ./build/cmsis-dap-read
```

On the connected STM32H7B0 it read 256 KiB through real CMSIS-DAP packets at
798.3 KiB/s with a 4096-byte packet size and produced the reference hash
`0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884`.

`cmsis-dap-tcp` exposes the same packet processor through the CMSIS-DAP TCP
framing used by clients that implement that transport. It listens on loopback
by default:

```sh
sudo ./build/cmsis-dap-tcp
```

`openocd/rpi-fast-cmsis-dap-tcp.cfg` records the expected OpenOCD configuration,
but the OpenOCD 0.12.0+dev-snapshot installed on the Pi rejects
`cmsis-dap backend tcp` before connecting. Therefore this is not currently a
working OpenOCD path on the Pi; it remains useful for an OpenOCD build that
actually includes that backend. This keeps the Pi as a normal Linux host and
avoids USB gadget mode entirely. `cmsis-dap-functionfs` and
`scripts/setup-functionfs.sh` remain historical experiments and are not part
of the GnWManager path.

For the native TCP path, configure a larger packet size at build time:

```sh
cmake -S . -B build-tcp -DCMAKE_BUILD_TYPE=Release \
  -DRPI_FAST_GPIO_CMSIS_DAP_PACKET_SIZE=4096
cmake --build build-tcp --target cmsis-dap-tcp
python3 scripts/cmsis-tcp-benchmark.py --packet-size 4096
```

On the connected target, this produced 786.1--786.7 KiB/s through the TCP
framing with the reference hash. A 1024-byte TCP packet size produced 753.2
KiB/s. The direct in-process CMSIS-DAP exerciser reached 798.3 KiB/s, so the
TCP framing costs roughly 1--2% with 4096-byte packets and more with 1024-byte
packets.

For an OpenOCD build with a CMSIS-DAP TCP backend, the stack would be:

```sh
# Terminal 1: fast GPIO probe, as root
sudo ./build/cmsis-dap-tcp

# Terminal 2: a recent OpenOCD with its CMSIS-DAP TCP backend
openocd -f openocd/rpi-fast-cmsis-dap-tcp.cfg \
  -f target/stm32h7x.cfg -c "gdb_port 3333"

# Terminal 3: GnWManager uses the local OpenOCD GDB server
gnwmanager --backend gdb --gdb-host 127.0.0.1 --gdb-port 3333 \
  serve --bind 0.0.0.0
```

The remote client connects to GnWManager's WebSocket endpoint, normally
`ws://<pi-address>:8765/gdb`; it never talks to the SWD pins or the CMSIS-DAP
TCP port directly.

## Native GDB/SWD path

Recovery loading requires a confirmed hardware halt. The native reset command
arms Cortex-M reset vector catch, waits for reset recovery, and checks DHCSR
before returning. A successful DHCSR write alone does not establish that the
core stayed halted: an outstanding reset can release it during a slower remote
upload or verification. GDB interrupt likewise waits for `S_HALT`.

SWD transfers retry only `WAIT`. A parity failure on an accepted posted AP read
must fail the block, allowing the client to restart from TAR; repeating that AP
request would advance the read pipeline and return shifted data. Failed GDB
sessions are detached so reconnect performs fresh SWD initialization.

`gdb-swd` removes OpenOCD from the Pi-side data path while keeping GnWManager's
GDB backend unchanged. It implements the memory, register, halt/resume, and
monitor operations used by that backend:

```sh
sudo ./build/gdb-swd --port 3333 --clock 128000000
gnwmanager --backend gdb --gdb-host 127.0.0.1 --gdb-port 3333 \
  serve --bind 0.0.0.0
```

The protocol smoke test is `scripts/gdb-swd-smoke.py`; on the connected target
it successfully read 64 KiB and completed a GDB detach. The GDB server batches
sequential requests by retaining the current MEM-AP TAR, and its response
framing is built and sent as one buffer rather than one syscall per byte. The
automated `scripts/throughput-matrix.sh` measured 673.7--679.4 KiB/s for
4--64 KiB GDB requests, with the reference hash, versus 797.6--798.2 KiB/s
for the direct GPIO path. The remaining difference is GDB's hex response
framing and host-side protocol overhead.

The server also accepts the standard binary memory-read packet (`x`). In the
same matrix it reached 684.6--695.7 KiB/s with the reference hash. This is
available to clients that choose binary GDB reads; GnWManager's existing GDB
backend uses the compatible hex `m` command.

To reproduce all transport comparisons in one run on the Pi:

```sh
bash scripts/throughput-matrix.sh
```

## SPI0 acceleration experiment

The verified SWD wiring also matches the Pi's SPI0 pins: GPIO11 is SPI0
SCLK and GPIO10 is the bidirectional SPI0 MOSI/MOMI pin. The `spi-swd-burst-read`
program is an experimental hardware-clocked backend using SPI0's bidirectional
(`REN`) mode; it does not require a GPIO9 jumper. SPI0 must first be enabled
in `/boot/firmware/config.txt` with `dtparam=spi=on`, followed by a reboot.
The standalone kernel-device probe is `spi3wire-idcode`; the direct-register
burst benchmark is `spi-swd-burst-read`. Neither is considered valid until it
returns `0x6ba02477` and a matching RAM hash on the connected target.

## Initial throughput measurements

On the connected STM32H7B0, reading 256 KiB from `0x24000000` initially reached 61.6 KiB/s with timed per-half-cycle waits. Removing timing calls once the GPIO path itself dominates, caching GPIO set/clear addresses, collapsing each SWD bit into one low-level operation, suppressing redundant SWDIO writes, and caching the SWDIO function-select values raised the CPU-driven result to 441.5 KiB/s at a 64 MHz requested clock. The printed clock is the requested value; high-rate mode is intentionally throughput-oriented and should be validated with a logic analyser before being treated as a precise clock source.

The repeatable sweep is `scripts/benchmark-sweep.sh`; it uses the same target address and 256 KiB transfer for every requested clock. The latest run produced:

| Requested SWCLK | Result |
|---:|---:|
| 1 MHz | 62.1 KiB/s |
| 2 MHz | 97.8 KiB/s |
| 4 MHz | 134.6 KiB/s |
| 6–64 MHz | 404.1–441.5 KiB/s |

The plateau above roughly 6 MHz shows that the current ceiling is per-transfer/protocol overhead rather than the requested SWCLK rate. The latest successive-halving tournament tested barrier variants, compiler options, CPU affinity, and requested clocks from 64–192 MHz. The validated GPIO loop reached 797.6–798.2 KiB/s in the consolidated matrix; pinning it to CPU 3 did not improve it. The native CMSIS-DAP `TransferBlock` path reached 796.4–798.0 KiB/s with the same full-memory hash, so the CMSIS-DAP layer is not the limiting factor.

The latest consolidated matrix reproduced that result with the target's full
256 KiB hash: direct fused GPIO reached 798.5 KiB/s, in-process CMSIS-DAP
reached 798.0 KiB/s, CMSIS-DAP TCP reached 788.5 KiB/s, and the native GDB
server reached 696.3 KiB/s using binary `x` packets. This confirms that the
remaining GDB loss is transport/protocol overhead rather than SWD correctness
or the CMSIS-DAP command processor.

## DMA waveform experiment

`dma-waveform` is a standalone raw-output test for the next backend stage. It uses the Pi's `vcsm-cma` allocator and DMA channel 5 to chain GPIO set/clear writes:

```sh
sudo ./build/dma-waveform --gpio 11 --pulses 100000
```

The first timing result was invalid because the benchmark treated the DMA
'END' flag as completion even while a linked-list transfer was still active.
The completion test now waits for 'ACTIVE' to clear. With that correction,
100,000 unpaced GPIO11 pulses complete at about 2.30 MHz. A PWM-FIFO-paced
chain completes at about 0.331 MHz for a nominal 1 MHz request and 0.774 MHz
for a nominal 3.989 MHz request. The latter is a valid timing demonstration,
but the four-control-block-per-pulse FIFO-feed chain has too much overhead to
be the final SWD engine. It does not generate SWDIO or sample target data; the
next step is a more compact synchronized output/input DMA schedule.

An earlier architecture-specific barrier variant was tested against the live
target and reached only 398.1 KiB/s; it is not the configuration described
below.

The validated ARM store/load barrier split is now the CMake default. The
fused reader also omits the barrier after a rising SWCLK edge and the barrier
between the falling edge and input sampling, plus the barrier after SWDIO data
writes. Barriers on the SWCLK falling edge and direction changes remain in
place. On the connected STM32H7B0, five consecutive full 256 KiB reads with
this configuration produced the same hash as the baseline at 795.4–796.7
KiB/s. Removing all barriers or removing only the remaining SWCLK store
barriers failed the correctness check.

The initial tournament swept SPI0's hardware-clocked `REN` path at dividers 2
through 200 and failed before the IDCODE check. After re-running the unchanged
probe, divider 200 and slower settings recovered the correct IDCODE, while
faster settings returned zero or corrupt responses. An independent diagnostic
confirmed that CPU-driven SPI output followed by GPIO input reads the correct
IDCODE, while the direct high-rate `REN` and kernel `SPI_3WIRE` paths remain
unusable for bulk transfers on this wiring.

The `spi-pipelined-read` experiment isolates the remaining promising portion:
the CPU GPIO backend performs SWD activation, MEM-AP setup, TAR programming,
and the posted-read prime, then SPI0 REN handles only read requests and data
responses. A full 256 KiB capture at divider 200 produced the reference hash
`0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884`, but only
178.7 KiB/s. Queueing all five SPI response bytes before draining the FIFO did
not change that result. This is a verified hybrid path, but it is rejected as
a performance backend because the CPU/SPI direction handoff dominates.

'dma-swd-idcode' is the first end-to-end DMA SWD proof. It emits the SWJ
activation sequence and DP IDCODE request, changes GPIO10 between output and
input through a DMA write to GPFSEL1, captures GPIO10 levels into DMA-visible
memory, and checks ACK, IDCODE parity, and the expected target value. On the
connected STM32H7B0 it returns '0x6ba02477'. The same builder now performs a
256-word (1 KiB) incrementing MEM-AP read and produces data matching the
reference CPU read. With DMA write responses enabled, the measured result is
124.5 KiB/s, 35,625 control blocks, and 9,483 GPIO samples. Disabling write
responses is correct but slower at 84.4 KiB/s. These figures use busy-poll DMA
completion timing; the earlier 1 ms sleep-poll measurement was too coarse.

This is still a proof of the waveform approach, not the final high-throughput
engine. At roughly 139 control blocks per 32-bit word, the current builder
cannot scale to the 256 KiB benchmark. The next performance step must replace
the per-bit GPIO/sample control-block schedule with hardware-paced streaming
or another representation that amortises the clock and capture operations.

The next experiment batched the PWM FIFO refill: one DMA control block supplied
1, 2, 4, 8, or 16 pacing words before the next GPIO/sample operation. A
50-case sweep covered 1, 5, 10, 20, and 40 MHz requested pacers, with zero or
four idle SWD cycles, using a 256-word read and checking the complete output
hash. Every zero-idle case failed at the same SWD sample; every four-idle case
was correct. The best result was only 69.5 KiB/s at 40 MHz, feed batch 16,
idle 4. This reduces control-block count but does not make the DMA architecture
competitive with the validated CPU reader, so further parameter tuning is
pruned.

The experimental `dma-stream-read` prototype tested that compact streaming
idea. A single PWM DREQ cannot pace both the GPIO-output and GPIO-sample DMA
channels: the channels arbitrate for the same requests and the captured stream
loses SWD alignment. A dual-PWM variant using the Pi 4's separate PWM1 DREQ
also failed the DP-IDCODE correctness check, even when paced at 100 Hz;
changing which DMA channel started first and changing the sample DREQ direction
had no effect. A shared-PWM version with output-priority arbitration and
capture delays from 5--100 microseconds also failed to produce an ACK. It is
retained as an experiment, not selected as the production backend; the
validated CPU-fused GPIO reader is currently the performance leader.

`dma-parallel-read` tested a more aggressive flat-stream variant. Separate
one-control-block DMA streams supplied GPCLR0, GPSET0, GPIO10 direction,
GPIO-level samples, and the PWM0 FIFO, all paced from the same PWM0 DREQ. The
streams completed from 1 MHz through 50 MHz DREQ, but the independent DMA
channels did not preserve the required GPIO write/sample ordering: no phase
produced a valid DP IDCODE, even at 1 MHz. It is retained as a rejected
architecture rather than a throughput result.

The remaining hardware-clocked SPI0 experiments are not faster. The direct
REN path can read the connected target correctly, but its best verified result
was 178.7 KiB/s with the GPIO10/GPIO9 jumper present. The kernel `SPI_3WIRE`
burst path did not obtain a valid IDCODE at 1--4 MHz. The CPU-fused GPIO path
therefore remains the highest-throughput correct implementation tested on this
Pi. With the Pi's CPU governor temporarily set to `performance` (fixed at the
configured 1.8 GHz maximum), the reproducible benchmark reached
797.4--798.8 KiB/s for three 256 KiB reads, each with the reference hash
above; the governor was restored to `ondemand` afterward.
Exceeding that result now requires either a firmware-level overclock or a
genuinely different execution architecture, not another small loop-tuning
sweep.

### GDB lifecycle compatibility (2026-10-13)

The server advertises a Cortex-M target description with `qXfer:features:read`.
Standard GDB uses xPSR register 25; the original minimal-client register 16 is
retained as an alias. Core register accesses halt the target first, while memory
reads remain passive. Debug halt freezes the STM32 watchdogs only while the core
is halted. Failed memory reads clear sticky debug-port errors without retrying
the invalid address, resetting the device, or closing the connection. GDB detach
resumes the target; disconnect alone does not.

Validated with `gdb-multiarch`: attach, monitor reset, a fully read-back-verified
48,492-byte recovery SRAM load, SP/PC writes, continue, Ctrl-C, IDLE/VTOR reads,
an invalid-address read followed by successful SRAM access, and detach. This
validates basic control and memory/register operations, not a full breakpoint or
single-stepping implementation.

Web-builder's managed GDB adapter now delegates reset-and-halt to GnWManager's
existing GDB workflow, rather than requiring guest DHCSR/vector-catch support.
The actual browser transport/flasher passed three recovery cycles on both the
Pi server and an isolated gwemu instance through unchanged installed GnWManager.
The hardware-working pre-change source/binary snapshot remains under
`snapshots/hardware-working-2026-10-13/`.
