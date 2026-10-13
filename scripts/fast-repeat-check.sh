#!/usr/bin/env bash
set -u

project=/home/doug/Nerd/git/rpi-fast-gpio
binary=/tmp/rpi-fast-gpio-fast-repeat
expected=0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884

cc -O3 -flto -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -DRPI_FAST_GPIO_STORE_BARRIERS \
    -DRPI_FAST_GPIO_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_INPUT_READ_BARRIERS \
    -DRPI_FAST_GPIO_NO_SWDIO_WRITE_BARRIERS \
    -DRPI_FAST_GPIO_NO_CLOCK_RISE_BARRIERS \
    -I"$project/include" \
    "$project/src/main.c" "$project/src/gpio_mmio.c" "$project/src/swd.c" \
    -o "$binary" || exit 1

printf 'run,hash,result\n'
for run in 1 2 3 4 5; do
    output="/tmp/rpi-fast-gpio-repeat-${run}.bin"
    log="/tmp/rpi-fast-gpio-repeat-${run}.log"
    if "$binary" --fast-gpio --clock 128000000 \
        --address 0x24000000 --length 262144 --output "$output" \
        >"$log" 2>&1; then
        hash=$(sha256sum "$output" | awk '{print $1}')
        result=$(sed -n 's/^read: /read:/p' "$log" | tail -n 1)
        if [ "$hash" = "$expected" ]; then
            printf '%s,%s,ok %s\n' "$run" "$hash" "$result"
        else
            printf '%s,%s,hash-mismatch %s\n' "$run" "$hash" "$result"
        fi
    else
        printf '%s,-,read-failed %s\n' "$run" "$(tail -n 1 "$log")"
    fi
done
