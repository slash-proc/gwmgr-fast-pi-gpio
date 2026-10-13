#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(pwd)}
address=${READ_ADDRESS:-0x24000000}
length=${READ_LENGTH:-262144}
expected=${EXPECTED_SHA256:-0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884}
cc_bin=${CC:-cc}

base_flags=(
    -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -DRPI_FAST_GPIO_STORE_BARRIERS
    -DRPI_FAST_GPIO_READ_BARRIERS
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS
    -DRPI_FAST_GPIO_NO_INPUT_READ_BARRIERS
    -DRPI_FAST_GPIO_NO_SWDIO_WRITE_BARRIERS
)

printf 'interval,status,rate_kib_s,sha256\n'
for interval in 1 2 4 8; do
    binary="/tmp/rpi-fast-barrier-${interval}"
    output="/tmp/rpi-fast-barrier-${interval}.bin"
    log="/tmp/rpi-fast-barrier-${interval}.log"
    if ! "$cc_bin" "${base_flags[@]}" \
        "-DRPI_FAST_GPIO_CLOCK_BARRIER_INTERVAL=${interval}" \
        -I"$project/include" \
        "$project/src/main.c" "$project/src/gpio_mmio.c" "$project/src/swd.c" \
        -o "$binary"; then
        printf '%s,compile-failed,,\n' "$interval"
        continue
    fi
    if sudo -n timeout 20s "$binary" --fast-gpio \
        --clock 128000000 --address "$address" --length "$length" \
        --output "$output" >"$log" 2>&1; then
        rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s.*/\1/p' "$log" | tail -n 1)
        hash=$(sha256sum "$output" | awk '{print $1}')
        status=hash-mismatch
        [[ "$hash" == "$expected" ]] && status=ok
        printf '%s,%s,%s,%s\n' "$interval" "$status" "$rate" "$hash"
    else
        printf '%s,run-failed,,\n' "$interval"
        tail -n 2 "$log"
    fi
done
