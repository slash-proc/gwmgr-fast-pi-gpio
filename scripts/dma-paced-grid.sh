#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(cd "$(dirname "$0")/.." && pwd)}
binary=/tmp/dma-swd-idcode-paced
words=${WORDS:-256}
expected=${EXPECTED_SHA256:-2b5f5fe2715a387fd2ba718bb6f4502b74a6e8c48bcae10cecdcd57e723f0c81}
timeout_s=${TIMEOUT_S:-5}

cc -O3 -flto -mcpu=cortex-a72 -mtune=cortex-a72 \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    "$project/src/dma_swd_idcode.c" -o "$binary"

printf 'pacer_hz,feed_batch,idle_cycles,status,rate_kib_s,sha256\n'
for pacer_hz in 1000000 5000000 10000000 20000000 40000000; do
    for feed_batch in 1 2 4 8 16; do
        for idle in 0 4; do
            output="/tmp/dma-paced-grid-${pacer_hz}-${feed_batch}-${idle}.bin"
            log="/tmp/dma-paced-grid-${pacer_hz}-${feed_batch}-${idle}.log"
            if sudo -n timeout "${timeout_s}s" "$binary" \
                --address 0x24000000 --words "$words" --pacer "$pacer_hz" \
                --feed-batch "$feed_batch" --idle "$idle" \
                --output "$output" >"$log" 2>&1; then
                rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s.*/\1/p' "$log" | tail -n 1)
                hash=$(sha256sum "$output" | awk '{print $1}')
                status=hash-mismatch
                [[ "$hash" == "$expected" ]] && status=ok
                printf '%s,%s,%s,%s,%s,%s\n' "$pacer_hz" "$feed_batch" "$idle" \
                    "$status" "$rate" "$hash"
            else
                printf '%s,%s,%s,run-failed,,\n' "$pacer_hz" "$feed_batch" "$idle"
                tail -n 2 "$log"
            fi
        done
    done
done
