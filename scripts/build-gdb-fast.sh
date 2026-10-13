#!/usr/bin/env bash

set -euo pipefail

project=${1:-$(pwd)}

arch_flags=()
if [[ "$(uname -m)" == "aarch64" || "$(uname -m)" == "arm64" ]]; then
    arch_flags=(-mcpu=cortex-a72 -mtune=cortex-a72)
fi

cc -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    "${arch_flags[@]}" \
    -DRPI_FAST_GPIO_STORE_BARRIERS \
    -DRPI_FAST_GPIO_READ_BARRIERS \
    -I"$project/include" \
    "$project/src/gdb_swd.c" "$project/src/gpio_dap.c" \
    "$project/src/gpio_mmio.c" "$project/src/cmsis_dap.c" \
    "$project/src/swd.c" -o /tmp/gdb-swd-fast
echo GDB_FAST_COMPILE_OK
