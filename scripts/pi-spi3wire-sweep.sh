#!/usr/bin/env bash
set -u

expected=0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884
for speed in 1000000 2000000 4000000 8000000 12000000 16000000 24000000 32000000; do
    output="/tmp/spi3wire-${speed}.bin"
    echo "=== speed=${speed} ==="
    if sudo -n timeout 20s /tmp/spi3wire-burst-read \
        --address 0x24000000 --words 256 --speed "$speed" --sync swj \
        --output "$output"; then
        hash=$(sha256sum "$output" | awk '{print $1}')
        if [[ "$hash" == "$expected" ]]; then
            echo "SPI3WIRE_HASH=ok $hash"
        else
            echo "SPI3WIRE_HASH=FAIL $hash expected=$expected"
        fi
    else
        echo "SPI3WIRE_RUN=failed speed=$speed"
    fi
done
