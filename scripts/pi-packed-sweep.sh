#!/usr/bin/env bash
set -u

expected=0af1975d460cc1ff8e94ce3fc3542ac4a14ecddc4133e2aed0ff7da1ac539884
for mode in word half byte; do
    output="/tmp/packed-${mode}.bin"
    echo "=== mode=${mode} ==="
    if sudo -n timeout 30s /tmp/packed-read --mode "$mode" \
        --clock 128000000 --address 0x24000000 --length 262144 \
        --output "$output"; then
        hash=$(sha256sum "$output" | awk '{print $1}')
        if [[ "$hash" == "$expected" ]]; then
            echo "PACKED_HASH=ok $hash"
        else
            echo "PACKED_HASH=FAIL $hash expected=$expected"
        fi
    else
        echo "PACKED_RUN=failed mode=$mode"
    fi
done
