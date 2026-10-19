#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(cd "$(dirname "$0")/.." && pwd)}
binary=/tmp/dma-swd-idcode-paced

cc -O3 -flto -mcpu=cortex-a72 -mtune=cortex-a72 \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    "$project/src/dma_swd_idcode.c" -o "$binary"

printf 'idle_cycles,status\n'
for idle in 0 1 2 4 8 16; do
    log="/tmp/dma-paced-idle-${idle}.log"
    if timeout 20s "$binary" \
        --address 0x24000000 --words 1 --pacer 10000000 \
        --idle "$idle" --output "/tmp/dma-paced-idle-${idle}.bin" \
        >"$log" 2>&1; then
        hash=$(sha256sum "/tmp/dma-paced-idle-${idle}.bin" | awk '{print $1}')
        printf '%s,hash=%s\n' "$idle" "$hash"
    else
        printf '%s,failed\n' "$idle"
        tail -n 3 "$log"
    fi
done
