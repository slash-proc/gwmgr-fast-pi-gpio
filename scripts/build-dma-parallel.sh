#!/usr/bin/env bash

set -euo pipefail

project=${1:-$(pwd)}
cc -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_READ_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS \
    -I"$project/include" \
    "$project/src/dma_parallel_read.c" "$project/src/gpio_mmio.c" \
    "$project/src/swd.c" -o /tmp/dma-parallel-read
echo DMA_PARALLEL_COMPILE_OK
