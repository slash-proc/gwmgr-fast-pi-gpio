#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(pwd)}
address=0x24000000
words=65536
expected=0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884

cc -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -I"$project/include" \
    "$project/src/spi3wire_burst_read.c" \
    "$project/src/gpio_mmio.c" "$project/src/swd.c" \
    -o /tmp/spi3wire-burst-read-tournament || exit 1
cc -O3 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -I"$project/include" \
    "$project/src/spi_swd_burst_read.c" \
    "$project/src/gpio_mmio.c" "$project/src/swd.c" \
    -o /tmp/spi-swd-burst-read-tournament || exit 1

printf 'engine,setting,status,rate_kib_s,sha256\n'

for speed in 8000000 12000000 20000000 24000000 32000000; do
    output="/tmp/spi3wire-tournament-${speed}.bin"
    log="/tmp/spi3wire-tournament-${speed}.log"
    status=run-failed
    rate=
    hash=
    if sudo -n timeout 20s /tmp/spi3wire-burst-read-tournament \
        --address "$address" --words "$words" --speed "$speed" \
        --sync swj --output "$output" >"$log" 2>&1; then
        rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s.*/\1/p' "$log" | tail -n 1)
        if hash=$(sha256sum "$output" | awk '{print $1}') &&
           [[ "$hash" == "$expected" ]]; then
            status=ok
        else
            status=hash-mismatch
        fi
    fi
    printf 'spi3wire,%s,%s,%s,%s\n' "$speed" "$status" "$rate" "$hash"
    if [[ "$status" != ok ]]; then
        tail -n 3 "$log" 2>/dev/null || true
    fi
done

for divider in 2 4 6 8 10 12 16 20 24 32; do
    output="/tmp/spi-mmio-tournament-${divider}.bin"
    log="/tmp/spi-mmio-tournament-${divider}.log"
    status=run-failed
    rate=
    hash=
    if sudo -n timeout 20s /tmp/spi-swd-burst-read-tournament \
        --words "$words" --divider "$divider" --retries 3 \
        --output "$output" >"$log" 2>&1; then
        rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s.*/\1/p' "$log" | tail -n 1)
        if hash=$(sha256sum "$output" | awk '{print $1}') &&
           [[ "$hash" == "$expected" ]]; then
            status=ok
        else
            status=hash-mismatch
        fi
    fi
    printf 'spi-mmio,%s,%s,%s,%s\n' "$divider" "$status" "$rate" "$hash"
    if [[ "$status" != ok ]]; then
        tail -n 3 "$log" 2>/dev/null || true
    fi
done
