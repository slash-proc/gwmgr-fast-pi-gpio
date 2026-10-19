#!/bin/bash

cc -O3 -Wall -Wextra -Wpedantic -Wconversion \
    -Iinclude \
    -DGWMGR_FAST_PI_GPIO_STORE_BARRIERS \
    -DGWMGR_FAST_PI_GPIO_READ_BARRIERS \
    src/main.c src/gpio_mmio.c src/swd.c \
    -o build/swd-read-opt
