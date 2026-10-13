#!/usr/bin/env bash
set -u

project_dir=${1:-/home/doug/Nerd/git/rpi-fast-gpio}
read_address=${READ_ADDRESS:-0x24000000}
read_length=${READ_LENGTH:-262144}
binary=${BINARY:-./build/swd-read-fast}

# This deliberately spans both paced and throughput-oriented regions of the
# current engine. Add values here when testing a new timing implementation.
clocks=(
    1000000 2000000 4000000 6000000 8000000 10000000
    12000000 16000000 24000000 32000000 40000000 48000000 64000000
)

cd "$project_dir" || exit 1

printf 'address=%s length=%s bytes\n' "$read_address" "$read_length"
printf '%-12s %s\n' 'requested_hz' 'result'
printf '%-12s %s\n' '------------' '------'

for clock in "${clocks[@]}"; do
    log_file="/tmp/rpi-fast-gpio-sweep-${clock}.log"

    if "$binary" \
        --clock "$clock" \
        --address "$read_address" \
        --length "$read_length" \
        >"$log_file" 2>&1; then
        result=$(tail -n 1 "$log_file")
        printf '%-12s %s\n' "$clock" "$result"
    else
        printf '%-12s FAILED\n' "$clock"
        tail -n 4 "$log_file"
    fi
done
