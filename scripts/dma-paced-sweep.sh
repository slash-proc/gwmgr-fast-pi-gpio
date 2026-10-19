#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(cd "$(dirname "$0")/.." && pwd)}
binary=/tmp/dma-swd-idcode-paced
words=${WORDS:-256}
expected=${EXPECTED_SHA256:-2b5f5fe2715a387fd2ba718bb6f4502b74a6e8c48bcae10cecdcd57e723f0c81}
feed_batch=${FEED_BATCH:-8}

cc -O3 -flto -mcpu=cortex-a72 -mtune=cortex-a72 \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    "$project/src/dma_swd_idcode.c" -o "$binary"

printf 'pacer_hz,status,rate_kib_s,sha256\n'
for pacer_hz in 1000000 5000000 10000000 20000000 40000000; do
    output="/tmp/dma-paced-${pacer_hz}.bin"
    log="/tmp/dma-paced-${pacer_hz}.log"
    if timeout 20s "$binary" \
        --address 0x24000000 --words "$words" --pacer "$pacer_hz" \
        --feed-batch "$feed_batch" \
        --output "$output" >"$log" 2>&1; then
        rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s.*/\1/p' "$log" | tail -n 1)
        hash=$(sha256sum "$output" | awk '{print $1}')
        status=hash-mismatch
        [[ "$hash" == "$expected" ]] && status=ok
        printf '%s,%s,%s,%s\n' "$pacer_hz" "$status" "$rate" "$hash"
    else
        printf '%s,run-failed,,\n' "$pacer_hz"
        tail -n 3 "$log"
    fi
done
