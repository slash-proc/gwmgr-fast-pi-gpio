#!/usr/bin/env bash
set -u

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project=$(cd -- "$script_dir/.." && pwd)
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
    local binary="/tmp/gwmgr-fast-pi-gpio-${name}"
    local output="/tmp/gwmgr-fast-pi-gpio-${name}.bin"
    local log="/tmp/gwmgr-fast-pi-gpio-${name}.log"

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
build_and_run barriers-generic -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
build_and_run barriers-a72 -mcpu=cortex-a72 \
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
build_and_run none-generic -DGWMGR_FAST_PI_GPIO_NO_BARRIERS
build_and_run none-a72 -mcpu=cortex-a72 -DGWMGR_FAST_PI_GPIO_NO_BARRIERS
build_and_run no-store-generic \
    -DGWMGR_FAST_PI_GPIO_NO_STORE_BARRIERS -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
build_and_run no-store-lto -flto \
    -DGWMGR_FAST_PI_GPIO_NO_STORE_BARRIERS -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
build_and_run barriers-lto -flto \
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
build_and_run no-rise-generic \
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS -DGWMGR_FAST_PI_GPIO_READ_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS
build_and_run no-rise-lto -flto \
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS -DGWMGR_FAST_PI_GPIO_READ_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS
