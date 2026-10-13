#!/usr/bin/env bash

set -u

project=/home/doug/Nerd/git/rpi-fast-gpio
expected=0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884
common=(
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -DRPI_FAST_GPIO_STORE_BARRIERS
    -DRPI_FAST_GPIO_READ_BARRIERS
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS
    -I"$project/include"
    "$project/src/main.c" "$project/src/gpio_mmio.c" "$project/src/swd.c"
)

printf 'CPU compiler sweep (fused GPIO, GPIO11/SWCLK, GPIO10/SWDIO)\n'
printf 'variant,result,hash\n'

build_and_run() {
    local name=$1
    shift
    local binary="/tmp/rpi-fast-gpio-${name}"
    local output="/tmp/rpi-fast-gpio-${name}.bin"
    local log="/tmp/rpi-fast-gpio-${name}.log"
    if ! cc "$@" "${common[@]}" -o "$binary"; then
        printf '%s,compile-failed,-\n' "$name"
        return
    fi
    if "$binary" --fast-gpio --clock 128000000 \
        --address 0x24000000 --length 262144 --output "$output" \
        >"$log" 2>&1; then
        local hash rate
        hash=$(sha256sum "$output" | awk '{print $1}')
        rate=$(sed -n 's/^read: /read:/p' "$log" | tail -n 1)
        if [ "$hash" = "$expected" ]; then
            printf '%s,ok,%s | %s\n' "$name" "$hash" "$rate"
        else
            printf '%s,hash-mismatch,%s | %s\n' "$name" "$hash" "$rate"
        fi
    else
        printf '%s,run-failed,- | %s\n' "$name" "$(tail -n 1 "$log")"
    fi
}

build_and_run baseline -O3 -flto
build_and_run unroll -O3 -flto -funroll-loops
build_and_run ofast -Ofast -flto
build_and_run no-vector -O3 -flto -fno-tree-vectorize
build_and_run aligned -O3 -flto -falign-loops=32 -falign-functions=32
build_and_run a72 -O3 -flto -mcpu=cortex-a72
