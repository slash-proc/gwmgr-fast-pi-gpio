#!/usr/bin/env bash

set -u
set -o pipefail

project=${1:-$(pwd)}
address_flags=(
    -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS
    -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS
    -DGWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS
)
if [[ "$(uname -m)" == "aarch64" || "$(uname -m)" == "arm64" ]]; then
    address_flags+=( -mcpu=cortex-a72 -mtune=cortex-a72 )
fi

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

printf 'packet_size,status,rate_kib_s,sha256\n'
for packet_size in 1024 4096; do
    binary="/tmp/cmsis-dap-read-${packet_size}"
    output=/tmp/cmsis-dap-read.bin
    log="/tmp/cmsis-dap-read-${packet_size}.log"
    if ! cc "${address_flags[@]}" \
        "-DGWMGR_FAST_PI_GPIO_CMSIS_DAP_PACKET_SIZE=${packet_size}U" \
        -I"$project/include" \
        "$project/src/cmsis_dap_read.c" "$project/src/cmsis_dap.c" \
        "$project/src/gpio_dap.c" "$project/src/gpio_mmio.c" \
        "$project/src/swd.c" -o "$binary"; then
        printf '%s,compile-failed,,\n' "$packet_size"
        continue
    fi
    if timeout 20s "$binary" >"$log" 2>&1; then
        rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s).*/\1/p' "$log" | tail -n 1)
        hash=$(sha256sum "$output" | awk '{print $1}')
        status=hash-mismatch
        [[ "$hash" == 0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884 ]] && status=ok
        printf '%s,%s,%s,%s\n' "$packet_size" "$status" "$rate" "$hash"
    else
        printf '%s,run-failed,,\n' "$packet_size"
        tail -n 2 "$log"
    fi
done
