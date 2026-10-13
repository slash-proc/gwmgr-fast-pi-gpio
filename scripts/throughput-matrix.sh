#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$project"

printf '=== direct fused GPIO ===\n'
bash scripts/benchmark-fastest.sh "$project" || true

printf '\n=== in-process CMSIS-DAP ===\n'
bash scripts/cmsis-block-size-sweep.sh "$project" || true

governor_file=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
original_governor=
restore_governor()
{
    if [[ -n "$original_governor" ]]; then
        sudo -n sh -c "echo '$original_governor' > '$governor_file'" || true
    fi
}
trap restore_governor EXIT
if [[ -r "$governor_file" ]]; then
    original_governor=$(<"$governor_file")
    sudo -n sh -c "echo performance > '$governor_file'" || true
fi

printf '\n=== native GDB/SWD ===\n'
bash scripts/build-gdb-fast.sh "$project"
sudo -n timeout 90s /tmp/gdb-swd-fast \
    --bind 127.0.0.1 --port 3334 --clock 128000000 \
    >/tmp/gdb-swd-matrix.log 2>&1 &
gdb_pid=$!
sleep 1
python3 scripts/gdb-block-size-benchmark.py 127.0.0.1 3334 || true
printf 'GDB binary-read packets (x):\n'
python3 scripts/gdb-block-size-benchmark.py 127.0.0.1 3334 --binary || true
kill "$gdb_pid" 2>/dev/null || true
wait "$gdb_pid" 2>/dev/null || true

printf '\n=== CMSIS-DAP TCP ===\n'
cc -O3 -flto -mcpu=cortex-a72 -mtune=cortex-a72 \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -DRPI_FAST_GPIO_STORE_BARRIERS \
    -DRPI_FAST_GPIO_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS \
    -DRPI_FAST_GPIO_NO_INPUT_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_SWDIO_WRITE_BARRIERS \
    -DRPI_FAST_GPIO_CMSIS_DAP_PACKET_SIZE=4096U \
    -Iinclude src/cmsis_dap_tcp.c src/cmsis_dap.c src/gpio_dap.c \
    src/gpio_mmio.c src/swd.c -o /tmp/cmsis-dap-tcp-matrix
sudo -n timeout 90s /tmp/cmsis-dap-tcp-matrix \
    --bind 127.0.0.1 --port 4442 --clock 128000000 \
    >/tmp/cmsis-dap-tcp-matrix.log 2>&1 &
cmsis_pid=$!
sleep 1
python3 scripts/cmsis-tcp-benchmark.py \
    --host 127.0.0.1 --port 4442 --packet-size 4096 || true
kill "$cmsis_pid" 2>/dev/null || true
wait "$cmsis_pid" 2>/dev/null || true
