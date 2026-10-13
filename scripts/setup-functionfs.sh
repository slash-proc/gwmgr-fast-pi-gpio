#!/usr/bin/env bash

set -euo pipefail

gadget=/sys/kernel/config/usb_gadget/rpi-fast-gpio
functionfs=/dev/ffs/cmsis-dap
config=${gadget}/configs/c.1
udc=${1:-}

modprobe libcomposite
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
mkdir -p "$gadget"
printf '0x0200\n' >"$gadget/bcdUSB"
printf '0x1209\n' >"$gadget/idVendor"
printf '0x0001\n' >"$gadget/idProduct"
mkdir -p "$gadget/strings/0x409"
printf 'rpi-fast-gpio\n' >"$gadget/strings/0x409/manufacturer"
printf 'Raspberry Pi CMSIS-DAPv2\n' >"$gadget/strings/0x409/product"
printf 'rpi-fast-gpio-0001\n' >"$gadget/strings/0x409/serialnumber"
mkdir -p "$config/strings/0x409"
printf 'CMSIS-DAPv2\n' >"$config/strings/0x409/configuration"
mkdir -p "$gadget/functions/ffs.cmsis-dap"
mkdir -p "$functionfs"
if ! mountpoint -q "$functionfs"; then
    mount -t functionfs cmsis-dap "$functionfs"
fi
link="$config/ffs.cmsis-dap"
if [ ! -e "$link" ]; then
    ln -s "$gadget/functions/ffs.cmsis-dap" "$link"
fi

if [ -z "$udc" ]; then
    udc=$(basename "$(find /sys/class/udc -mindepth 1 -maxdepth 1 -print -quit)")
fi
if [ -z "$udc" ]; then
    echo 'No USB device controller found; is the Pi USB-C port in device mode?' >&2
    exit 1
fi

printf 'FunctionFS prepared at %s\n' "$functionfs"
printf 'Start build/cmsis-dap-functionfs as root, then bind with:\n'
printf '  printf %s > %s/UDC\n' "$udc" "$gadget"
