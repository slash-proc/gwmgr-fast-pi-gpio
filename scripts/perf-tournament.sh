#!/usr/bin/env bash

# Successive-halving benchmark for the real Pi/target pair. Hardware tests are
# serialized because every candidate owns the same SWD pins and target.

set -u
set -o pipefail

project=${PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
address=${READ_ADDRESS:-0x24000000}
length=${READ_LENGTH:-262144}
expected=${EXPECTED_SHA256:-0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884}
report=${REPORT:-/tmp/gwmgr-fast-pi-gpio-performance.csv}
cc_bin=${CC:-cc}
tmpdir=$(mktemp -d /tmp/gwmgr-fast-pi-gpio-tournament.XXXXXX)
stage1="$tmpdir/stage1.tsv"

cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

printf 'engine,variant,affinity,clock_hz,status,rate_kib_s,sha256\n' >"$report"
printf 'report=%s address=%s length=%s expected=%s\n' \
       "$report" "$address" "$length" "$expected"

base_flags=(
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -O3 -flto
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS
    -DGWMGR_FAST_PI_GPIO_READ_BARRIERS
)
cpu_sources=(
    "$project/src/main.c"
    "$project/src/gpio_mmio.c"
    "$project/src/swd.c"
)
variant_names=(validated full-barrier no-rise no-input-read no-swdio-write no-fsel unrolled cortex-a72)

variant_flags() {
    case "$1" in
        validated)
            printf '%s\n' -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS \
                -DGWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS \
                -DGWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS ;;
        full-barrier)
            printf '%s\n' -DGWMGR_FAST_PI_GPIO_FULL_BARRIERS ;;
        no-rise)
            printf '%s\n' -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS ;;
        no-input-read)
            printf '%s\n' -DGWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS ;;
        no-swdio-write)
            printf '%s\n' -DGWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS ;;
        no-fsel)
            printf '%s\n' -DGWMGR_FAST_PI_GPIO_NO_FSEL_BARRIERS ;;
        unrolled)
            printf '%s\n' -funroll-loops ;;
        cortex-a72)
            printf '%s\n' -mcpu=cortex-a72 ;;
        *) return 1 ;;
    esac
}

run_command() {
    local affinity=$1
    shift
    if [[ "$affinity" == none ]]; then
        "$@"
    else
        taskset --cpu-list "$affinity" "$@"
    fi
}

rate_from_log() {
    sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s.*/\1/p' "$1" | tail -n 1
}

hash_for() {
    sha256sum "$1" | awk '{print $1}'
}

declare -A cpu_binaries
: >"$stage1"

run_cpu_candidate() {
    local name=$1
    local affinity=$2
    local clock=$3
    local binary=${cpu_binaries[$name]:-}
    local output="$tmpdir/${name}-${affinity}-${clock}.bin"
    local log="$tmpdir/${name}-${affinity}-${clock}.log"

    if [[ -z "$binary" ]]; then
        binary="$tmpdir/cpu-${name}"
        mapfile -t extra_flags < <(variant_flags "$name") || return 1
        if ! "$cc_bin" "${base_flags[@]}" "${extra_flags[@]}" \
            -I"$project/include" "${cpu_sources[@]}" -o "$binary"; then
            printf 'cpu,%s,%s,%s,compile-failed,,\n' "$name" "$affinity" "$clock" >>"$report"
            return 1
        fi
        cpu_binaries[$name]=$binary
    fi

    if run_command "$affinity" timeout 20s "$binary" --fast-gpio \
        --clock "$clock" --address "$address" --length "$length" \
        --output "$output" >"$log" 2>&1; then
        local hash rate status
        hash=$(hash_for "$output")
        rate=$(rate_from_log "$log")
        status=hash-mismatch
        if [[ "$hash" == "$expected" && -n "$rate" ]]; then
            status=ok
            printf '%s\t%s\n' "$name" "$rate" >>"$stage1"
        fi
        printf 'cpu,%s,%s,%s,%s,%s,%s\n' \
               "$name" "$affinity" "$clock" "$status" "$rate" "$hash" >>"$report"
        printf 'cpu %-16s affinity=%-4s clock=%-9s status=%-14s rate=%s hash=%s\n' \
               "$name" "$affinity" "$clock" "$status" "${rate:--}" "$hash"
    else
        printf 'cpu,%s,%s,%s,run-failed,,\n' "$name" "$affinity" "$clock" >>"$report"
        printf 'cpu %-16s affinity=%-4s clock=%-9s status=run-failed %s\n' \
               "$name" "$affinity" "$clock" "$(tail -n 1 "$log")"
    fi
}

printf '\nStage 1: native CPU variant screen\n'
for name in "${variant_names[@]}"; do
    run_cpu_candidate "$name" none 128000000
done

mapfile -t survivors < <(sort -k2,2nr "$stage1" | awk '!seen[$1]++' | head -n 3 | cut -f1)
if ((${#survivors[@]} == 0)); then
    echo 'No correct CPU candidate survived stage 1.' >&2
    exit 1
fi

printf '\nStage 2: clock and affinity sweep for survivors: %s\n' "${survivors[*]}"
for name in "${survivors[@]}"; do
    for affinity in none 3; do
        for clock in 64000000 96000000 128000000 160000000 192000000; do
            run_cpu_candidate "$name" "$affinity" "$clock"
        done
    done
done

printf '\nStage 3: native CMSIS-DAP TransferBlock path\n'
dap_binary="$tmpdir/cmsis-dap-read"
if "$cc_bin" "${base_flags[@]}" \
    -DGWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS \
    -I"$project/include" \
    "$project/src/cmsis_dap_read.c" "$project/src/cmsis_dap.c" \
    "$project/src/gpio_dap.c" "$project/src/gpio_mmio.c" "$project/src/swd.c" \
    -o "$dap_binary"; then
    dap_log="$tmpdir/cmsis-dap-read.log"
    if run_command none timeout 20s "$dap_binary" >"$dap_log" 2>&1; then
        dap_hash=$(hash_for /tmp/cmsis-dap-read.bin)
        dap_rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s).*/\1/p' "$dap_log" | tail -n 1)
        dap_status=hash-mismatch
        [[ "$dap_hash" == "$expected" ]] && dap_status=ok
        printf 'cmsis-dap,transfer-block,none,128000000,%s,%s,%s\n' \
               "$dap_status" "$dap_rate" "$dap_hash" >>"$report"
        printf 'cmsis-dap transfer-block status=%s rate=%s hash=%s\n' \
               "$dap_status" "${dap_rate:--}" "$dap_hash"
    else
        printf 'cmsis-dap,transfer-block,none,128000000,run-failed,,\n' >>"$report"
        tail -n 3 "$dap_log"
    fi
else
    printf 'cmsis-dap,transfer-block,none,128000000,compile-failed,,\n' >>"$report"
fi

printf '\nStage 4: SPI0 three-wire divider sweep\n'
spi_binary="$tmpdir/spi-swd-burst-read"
if "$cc_bin" -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -I"$project/include" \
    "$project/src/spi_swd_burst_read.c" "$project/src/gpio_mmio.c" \
    "$project/src/swd.c" -o "$spi_binary"; then
    spi_words=$((length / 4))
    for divider in 2 4 6 8 10 12 16 20 24 32 40 50 64 80 100 128 160 200; do
        spi_output="$tmpdir/spi-${divider}.bin"
        spi_log="$tmpdir/spi-${divider}.log"
        if run_command none timeout 20s "$spi_binary" \
            --address "$address" --words "$spi_words" --divider "$divider" \
            --retries 3 --output "$spi_output" >"$spi_log" 2>&1; then
            spi_hash=$(hash_for "$spi_output")
            spi_rate=$(sed -n 's/.*(\([0-9][0-9.]*\) KiB\/s.*/\1/p' "$spi_log" | tail -n 1)
            spi_status=hash-mismatch
            [[ "$spi_hash" == "$expected" ]] && spi_status=ok
            printf 'spi0,divider-%s,none,,%s,%s,%s\n' \
                   "$divider" "$spi_status" "$spi_rate" "$spi_hash" >>"$report"
            printf 'spi0 divider=%-3s status=%-14s rate=%s hash=%s\n' \
                   "$divider" "$spi_status" "${spi_rate:--}" "$spi_hash"
        else
            printf 'spi0,divider-%s,none,,run-failed,,\n' "$divider" >>"$report"
            printf 'spi0 divider=%-3s status=run-failed %s\n' \
                   "$divider" "$(tail -n 1 "$spi_log")"
        fi
    done
else
    printf 'spi0,all,none,,compile-failed,,\n' >>"$report"
fi

printf '\nComplete report: %s\n' "$report"
