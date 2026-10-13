#!/usr/bin/env bash
set -euo pipefail

project=${1:-$(pwd)}
cc -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -DRPI_FAST_GPIO_STORE_BARRIERS \
    -DRPI_FAST_GPIO_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS \
    -DRPI_FAST_GPIO_NO_INPUT_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_SWDIO_WRITE_BARRIERS \
    -I"$project/include" \
    "$project/src/packed_read.c" "$project/src/gpio_mmio.c" \
    "$project/src/swd.c" -o /tmp/packed-read
echo PACKED_READ_COMPILE_OK
