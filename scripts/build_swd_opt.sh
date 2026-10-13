#!/bin/bash

cc -O3 -Wall -Wextra -Wpedantic -Wconversion \
    -Iinclude \
    -DRPI_FAST_GPIO_STORE_BARRIERS \
    -DRPI_FAST_GPIO_READ_BARRIERS \
    src/main.c src/gpio_mmio.c src/swd.c \
    -o build/swd-read-opt
