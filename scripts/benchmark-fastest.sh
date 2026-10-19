#!/usr/bin/env bash
set -euo pipefail

project_dir=${1:-$(cd "$(dirname "$0")/.." && pwd)}
address=${READ_ADDRESS:-0x24000000}
length=${READ_LENGTH:-262144}
runs=${RUNS:-3}
output=${OUTPUT_FILE:-/tmp/gwmgr-fast-pi-gpio-fastest.bin}
expected_hash=0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884
binary=/tmp/gwmgr-fast-pi-gpio-swd-read-fastest

cd "$project_dir"

target_flags=(
    -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS
    -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS
)
if [[ "$(uname -m)" == "aarch64" || "$(uname -m)" == "arm64" ]]; then
    target_flags+=( -mcpu=cortex-a72 -mtune=cortex-a72 )
fi

cc "${target_flags[@]}" -Iinclude \
    src/main.c src/gpio_mmio.c src/swd.c -o "$binary"

governor_file=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
original_governor=
restore_governor()
{
    if [[ -n "$original_governor" && -w "$governor_file" ]]; then
        printf '%s\n' "$original_governor" >"$governor_file" || true
    fi
}
trap restore_governor EXIT

if [[ "${PERFORMANCE_GOVERNOR:-1}" == 1 && -r "$governor_file" && -w "$governor_file" ]]; then
    original_governor=$(<"$governor_file")
    printf 'performance\n' >"$governor_file"
fi

printf 'run,rate_kib_s,sha256\n'
for run in $(seq 1 "$runs"); do
    log_file=/tmp/gwmgr-fast-pi-gpio-fastest-${run}.log
    timeout 30s "$binary" --fast-gpio \
        --clock 128000000 --address "$address" --length "$length" \
        --output "$output" >"$log_file" 2>&1
    rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s).*/\1/p' "$log_file" | tail -n 1)
    hash=$(sha256sum "$output" | awk '{print $1}')
    if [[ "$hash" != "$expected_hash" ]]; then
        echo "hash mismatch on run $run: $hash" >&2
        cat "$log_file" >&2
        exit 1
    fi
    printf '%s,%s,%s\n' "$run" "$rate" "$hash"
done
