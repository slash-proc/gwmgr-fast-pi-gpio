#!/usr/bin/env bash
set -u

project=/home/doug/Nerd/git/rpi-fast-gpio
address=0x24000000
length=262144
clock=128000000
fast_args=()
if [ "${FAST_GPIO:-0}" = 1 ]; then
    fast_args=(--fast-gpio)
fi

printf 'CPU SWD variant sweep (GPIO11/SWCLK, GPIO10/SWDIO)\n'
if [ "${FAST_GPIO:-0}" = 1 ]; then
    printf 'engine=fused-gpio\n'
else
    printf 'engine=generic-interface\n'
fi
printf 'variant,result,hash\n'

build_and_run() {
    local name="$1"
    shift
    local binary="/tmp/rpi-fast-gpio-${name}"
    local output="/tmp/rpi-fast-gpio-${name}.bin"
    local log="/tmp/rpi-fast-gpio-${name}.log"

    if ! cc -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
        "$@" -I"$project/include" \
        "$project/src/main.c" "$project/src/gpio_mmio.c" "$project/src/swd.c" \
        -o "$binary"; then
        printf '%s,compile-failed,-\n' "$name"
        return
    fi
    if "$binary" --clock "$clock" --address "$address" --length "$length" \
        --output "$output" "${fast_args[@]}" >"$log" 2>&1; then
        local rate hash
        rate=$(sed -n 's/^read: /read:/p' "$log" | tail -n 1)
        hash=$(sha256sum "$output" | awk '{print $1}')
        printf '%s,ok,%s | %s\n' "$name" "$hash" "$rate"
    else
        printf '%s,run-failed,- | %s\n' "$name" "$(tail -n 1 "$log")"
    fi
}

build_and_run full-generic
build_and_run full-a72 -mcpu=cortex-a72
build_and_run barriers-generic -DRPI_FAST_GPIO_STORE_BARRIERS -DRPI_FAST_GPIO_READ_BARRIERS
build_and_run barriers-a72 -mcpu=cortex-a72 \
    -DRPI_FAST_GPIO_STORE_BARRIERS -DRPI_FAST_GPIO_READ_BARRIERS
build_and_run none-generic -DRPI_FAST_GPIO_NO_BARRIERS
build_and_run none-a72 -mcpu=cortex-a72 -DRPI_FAST_GPIO_NO_BARRIERS
build_and_run no-store-generic \
    -DRPI_FAST_GPIO_NO_STORE_BARRIERS -DRPI_FAST_GPIO_READ_BARRIERS
build_and_run no-store-lto -flto \
    -DRPI_FAST_GPIO_NO_STORE_BARRIERS -DRPI_FAST_GPIO_READ_BARRIERS
build_and_run barriers-lto -flto \
    -DRPI_FAST_GPIO_STORE_BARRIERS -DRPI_FAST_GPIO_READ_BARRIERS
build_and_run no-rise-generic \
    -DRPI_FAST_GPIO_STORE_BARRIERS -DRPI_FAST_GPIO_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS
build_and_run no-rise-lto -flto \
    -DRPI_FAST_GPIO_STORE_BARRIERS -DRPI_FAST_GPIO_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS
