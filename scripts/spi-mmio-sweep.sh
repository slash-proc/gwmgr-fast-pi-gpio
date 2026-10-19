#!/usr/bin/env bash
set -u

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project=$(cd -- "$script_dir/.." && pwd)
binary="$project/build/spi-swd-burst-read"

cc -O3 -I"$project/include" -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -o "$binary" \
    "$project/src/spi_swd_burst_read.c" \
    "$project/src/gpio_mmio.c" \
    "$project/src/swd.c" || exit 1

printf 'direct SPI-register SWD sweep (GPIO11/SCLK, GPIO10/MOSI/MOMI)\n'
printf 'divider,status,result\n'

for divider in 2 4 6 8 10 12 16 20 24 32 40 50 64 80 100 128 160 200; do
    if result=$(timeout 4s "$binary" --words 256 --divider "$divider" \
        --retries 1 2>&1); then
        printf '%s,ok,%s\n' "$divider" "$(printf '%s\n' "$result" | tail -n 1)"
    else
        printf '%s,fail,%s\n' "$divider" "$(printf '%s\n' "$result" | tail -n 1)"
        printf '%s\n' "$result" | tail -n 3
    fi
done
