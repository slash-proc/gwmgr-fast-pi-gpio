#!/bin/bash

set -u

expected='2b5f5fe2715a387fd2ba718bb6f4502b74a6e8c48bcae10cecdcd57e723f0c81'
for idle in 0 8 16 32 56 80; do
    output="/tmp/dma-optimized-${idle}.bin"
    echo "=== post-idle-cycles=${idle} ==="
    ./build/dma-swd-idcode \
        --address 0x24000000 \
        --words 256 \
        --idle "${idle}" \
        --output "${output}"
    hash=$(sha256sum "${output}" | awk '{print $1}')
    if [[ "${hash}" == "${expected}" ]]; then
        echo "DMA_HASH=ok ${hash}"
    else
        echo "DMA_HASH=FAIL ${hash} expected=${expected}"
    fi
done
