#!/usr/bin/env bash
set -u

expected=2b5f5fe2715a387fd2ba718bb6f4502b74a6e8c48bcae10cecdcd57e723f0c81
for hz in 5000000 10000000 20000000 40000000 80000000; do
    output="/tmp/dma-parallel-${hz}.bin"
    echo "=== dreq=${hz} ==="
    if sudo -n timeout 20s /tmp/dma-parallel-read \
        --address 0x24000000 --words 256 --dreq "$hz" --output "$output"; then
        hash=$(sha256sum "$output" | awk '{print $1}')
        if [[ "$hash" == "$expected" ]]; then
            echo "DMA_PARALLEL_HASH=ok $hash"
        else
            echo "DMA_PARALLEL_HASH=FAIL $hash expected=$expected"
        fi
    else
        echo "DMA_PARALLEL_RUN=failed dreq=$hz"
    fi
done
