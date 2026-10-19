#!/bin/bash

set -u

for divider in 20 40 80 100 200 400 800; do
    echo "=== spi-divider=${divider} ==="
    ./build/spi-swd-probe --divider "${divider}"
done
