#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(cd "$(dirname "$0")/.." && pwd)}
address=${READ_ADDRESS:-0x24000000}
length=${READ_LENGTH:-262144}
expected=${EXPECTED_SHA256:-0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884}
runs=${RUNS:-3}

flags=(
    -O3 -flto -mcpu=cortex-a72 -mtune=cortex-a72
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS
    -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS
)

printf 'mapping,run,status,rate_kib_s,sha256\n'
for mapping in gpiomem devmem; do
    binary="/tmp/swd-read-${mapping}"
    extra=()
    [[ "$mapping" == devmem ]] && extra=(-DGWMGR_FAST_PI_GPIO_USE_DEVMEM)
    if ! cc "${flags[@]}" "${extra[@]}" -I"$project/include" \
        "$project/src/main.c" "$project/src/gpio_mmio.c" "$project/src/swd.c" \
        -o "$binary"; then
        printf '%s,0,compile-failed,,\n' "$mapping"
        continue
    fi
    for run in $(seq 1 "$runs"); do
        output="/tmp/mmio-${mapping}-${run}.bin"
        log="/tmp/mmio-${mapping}-${run}.log"
        if timeout 30s "$binary" --fast-gpio --clock 128000000 \
            --address "$address" --length "$length" --output "$output" \
            >"$log" 2>&1; then
            rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s).*/\1/p' "$log" | tail -n 1)
            hash=$(sha256sum "$output" | awk '{print $1}')
            status=hash-mismatch
            [[ "$hash" == "$expected" ]] && status=ok
            printf '%s,%s,%s,%s,%s\n' "$mapping" "$run" "$status" "$rate" "$hash"
        else
            printf '%s,%s,run-failed,,\n' "$mapping" "$run"
            tail -n 2 "$log"
        fi
    done
done
