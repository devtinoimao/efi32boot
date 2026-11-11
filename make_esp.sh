#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build a generic FAT32 EFI System Partition for the hosted-firmware loader.
#
# The loader is OS-neutral: it hosts an inner firmware which boots whatever
# EFI OS is present on this ESP or any other device.  No OS-specific file is
# named, copied or removed here.
#
# Usage:
#   ./make_esp.sh [OS_TREE_DIR] [OUTPUT_IMAGE]
#
#   OS_TREE_DIR   optional directory whose contents are mirrored onto the ESP.
#                 Use this to add any 32-bit EFI OS bootloader / kernel tree
#                 (e.g. a Linux \EFI\BOOT\BOOTIA32.EFI, a BSD loader, or a
#                 Windows boot manager).  It is copied verbatim.
#   OUTPUT_IMAGE  defaults to ./build/esp.img (project-relative)
#
# Environment:
#   ESP_SIZE_MB   image size in MiB (default 1024)
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")" && pwd)"
OS_TREE="${1:-}"
IMG="${2:-${ESP_IMG:-$PROJECT/build/esp.img}}"
SIZE_MB="${ESP_SIZE_MB:-1024}"

for tool in dd mkfs.vfat sfdisk mmd mcopy mdir; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

mkdir -p "$(dirname "$IMG")"
echo "==> creating ${SIZE_MB} MiB FAT32 image: $IMG"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none

# A real MBR partition table is required.  A bare FAT32 volume is seen as
# removable media, which means no EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and no way
# for the firmware to enumerate boot options from it.
echo "==> partitioning (MBR, one FAT32 partition)"
sfdisk --no-reread --no-tell-kernel "$IMG" <<'PART' >/dev/null
label: dos
unit: sectors
start=2048, type=c, bootable
PART

echo "==> creating the filesystem"
mkfs.vfat -F 32 -n ESP --offset 2048 "$IMG" >/dev/null
VOL="$IMG@@1M"

mmd -i "$VOL" ::/EFI
mmd -i "$VOL" ::/EFI/BOOT

# The loader itself.  BOOTX64.EFI is the standard removable-media path for a
# 64-bit host firmware; the inner 32-bit firmware will look for its own
# architecture's default path (BOOTIA32.EFI) or any configured Boot#### option.
if [ -f "$PROJECT/BOOTX64.EFI" ]; then
    echo "==> copying BOOTX64.EFI"
    mcopy -i "$VOL" "$PROJECT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
else
    echo "!!  $PROJECT/BOOTX64.EFI not found - run 'make' first" >&2
fi

# Optional: mirror any OS tree the caller supplies.  No assumptions about its
# layout, filenames or OS.  Skip the loader path so we never overwrite it.
if [ -n "$OS_TREE" ]; then
    [ -d "$OS_TREE" ] || { echo "not a directory: $OS_TREE" >&2; exit 1; }
    echo "==> mirroring OS tree: $OS_TREE"
    (cd "$OS_TREE" && find . -mindepth 1 -type d -print) | while read -r d; do
        target=${d#./}
        mmd -i "$VOL" "::/$target" 2>/dev/null || true
    done
    (cd "$OS_TREE" && find . -mindepth 1 -type f -print) | while read -r f; do
        src=${f#./}
        case "$(printf '%s' "$src" | tr '[:lower:]' '[:upper:]')" in
            EFI/BOOT/BOOTX64.EFI) echo "    skip $src (reserved for the loader)" ;;
            *) mcopy -i "$VOL" "$OS_TREE/$src" "::/$src" ;;
        esac
    done
fi

echo "==> ESP layout"
mdir -i "$VOL" -/ ::/EFI/BOOT
echo "image: $IMG"
