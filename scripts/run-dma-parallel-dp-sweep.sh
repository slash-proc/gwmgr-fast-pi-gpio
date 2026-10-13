#!/usr/bin/env bash

set -u
for rate in 1000000 2000000 5000000 10000000 20000000 30000000 40000000 50000000; do
    printf 'DREQ=%s\n' "$rate"
    sudo -n timeout 5s /tmp/dma-parallel-read --dp --dreq "$rate"
done
