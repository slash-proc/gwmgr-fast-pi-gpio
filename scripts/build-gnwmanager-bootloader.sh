#!/usr/bin/env bash
set -euo pipefail

# Build the local triple-boot bootloader in the layout used by GnWmanager's
# --bootloader flash-patch mode.  This script does not install or flash it
# unless --install is explicitly supplied.

repo_dir=${GNW_BOOTLOADER_REPO:-"$(cd "$(dirname "$0")/../../game-and-watch-bootloader" && pwd)"}
cache_dir=${GNW_BOOTLOADER_CACHE:-"$HOME/.cache/gnwmanager/bootloader/sylverb/game-and-watch-bootloader"}
output_dir=$(mktemp -d)
trap 'rm -rf "$output_dir"' EXIT

make -C "$repo_dir" DEBUG=0 INTFLASH_ADDRESS=0x08032000 BUILD_DIR="$output_dir/build"

image="$output_dir/build/gnw_bootloader.bin"
size=$(stat -c '%s' "$image")
max_size=$((0x40000 - 0x32000))
if (( size > max_size )); then
    printf 'bootloader is %d bytes; maximum at 0x08032000 is %d bytes\n' "$size" "$max_size" >&2
    exit 1
fi

if [[ ${1:-} == "--install" ]]; then
    tag=${GNW_BOOTLOADER_TAG:-v1.0.8}
    destination="$cache_dir/${tag}_0x08032000.bin"
    mkdir -p "$cache_dir"
    if [[ -e "$destination" && ! -e "$destination.release" ]]; then
        cp -p "$destination" "$destination.release"
    fi
    cp -p "$image" "$destination"
    printf 'installed %s (%d bytes)\n' "$destination" "$size"
else
    cp -p "$image" "${GNW_BOOTLOADER_OUTPUT:-./gnw_bootloader_0x08032000.bin}"
    printf 'wrote %s (%d bytes)\n' "${GNW_BOOTLOADER_OUTPUT:-./gnw_bootloader_0x08032000.bin}" "$size"
fi
