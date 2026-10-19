#!/bin/bash

set -u

for idle in 0 1 2 4 8 16 32 56; do
    echo "=== post-idle-cycles=${idle} ==="
    ./build/dma-swd-idcode \
        --address 0x24000000 \
        --words 1 \
        --idle "${idle}" 2>&1 |
        grep -E 'DMA SWD (memory response|IDCODE|read:|diagnostic sample 36)'
done
