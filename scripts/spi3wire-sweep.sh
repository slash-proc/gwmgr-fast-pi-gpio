#!/usr/bin/env bash
set -u

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project=$(cd -- "$script_dir/.." && pwd)
binary="$project/build/spi3wire-burst-read"

cc -O3 -I"$project/include" -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -o "$binary" \
    "$project/src/spi3wire_burst_read.c" \
    "$project/src/gpio_mmio.c" \
    "$project/src/swd.c" || exit 1

printf 'SPI handoff matrix (GPIO11/SCLK, GPIO10/MOSI/MOMI)\n'
printf 'submission,sync,speed,id,dp,ap,burst\n'

for submission in separate combined; do
    for sync in none idle1 idle7 line-reset swj swj-reset; do
        for speed in 1000000 4000000 8000000 12000000 20000000; do
            probe_flag=
            if [ "$submission" = combined ]; then
                probe_flag=--combined-probe
            fi
            result=$(timeout 12s "$binary" \
                --address 0x24000000 \
                --words 1 \
                --speed "$speed" \
                --sync "$sync" \
                --probe-only $probe_flag 2>/dev/null || true)
            summary=$(printf '%s\n' "$result" | sed -n 's/^SPI_RESULT //p' | tail -n 1)
            if [ -n "$summary" ]; then
                printf '%s\n' "$summary" | awk -v submission="$submission" '{
                sync=""; speed=""; id=""; dp=""; ap=""; burst="";
                for (i = 1; i <= NF; ++i) {
                    split($i, field, "=");
                    if (field[1] == "sync") sync=field[2];
                    if (field[1] == "speed") speed=field[2];
                    if (field[1] == "id") id=field[2];
                    if (field[1] == "dp") dp=field[2];
                    if (field[1] == "ap") ap=field[2];
                    if (field[1] == "burst") burst=field[2];
                }
                printf "%s,%s,%s,%s,%s,%s,%s\n", submission, sync, speed, id, dp, ap, burst;
                }'
            else
                printf '%s,%s,%s,launch-failed,launch-failed,launch-failed,not-run\n' \
                    "$submission" "$sync" "$speed"
            fi
        done
    done
done
