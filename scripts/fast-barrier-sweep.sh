#!/usr/bin/env bash

set -u

project=/home/doug/Nerd/git/rpi-fast-gpio
expected=0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884
common=(
    -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -DRPI_FAST_GPIO_STORE_BARRIERS
    -DRPI_FAST_GPIO_READ_BARRIERS
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS
    -I"$project/include"
    "$project/src/main.c" "$project/src/gpio_mmio.c" "$project/src/swd.c"
)

printf 'CPU barrier sweep (fused GPIO, GPIO11/SWCLK, GPIO10/SWDIO)\n'
printf 'variant,result,hash\n'

build_and_run() {
    local name=$1
    shift
    local binary="/tmp/rpi-fast-gpio-barrier-${name}"
    local output="/tmp/rpi-fast-gpio-barrier-${name}.bin"
    local log="/tmp/rpi-fast-gpio-barrier-${name}.log"
    if ! cc "${common[@]}" "$@" -o "$binary"; then
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

build_and_run baseline
build_and_run no-input-read -DRPI_FAST_GPIO_NO_INPUT_READ_BARRIERS
build_and_run no-fsel -DRPI_FAST_GPIO_NO_INPUT_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_FSEL_BARRIERS
build_and_run no-swdio-write -DRPI_FAST_GPIO_NO_INPUT_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_SWDIO_WRITE_BARRIERS
