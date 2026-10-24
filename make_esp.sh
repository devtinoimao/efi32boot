#!/usr/bin/env bash
# Build the FAT32 ESP that carries both the 64 bit bootloader and the
# WinPE x86 media for the hosted 32 bit firmware.
set -euo pipefail

ISO="${1:-$HOME/Downloads/winpese-x86_16.07.07.iso}"
WORK=/tmp/opencode
WINPE=$WORK/winpe
IMG=$WORK/esp.img
PROJECT="$(cd "$(dirname "$0")" && pwd)"

echo "==> extracting $ISO"
chmod -R u+w "$WINPE" 2>/dev/null || true
rm -rf "$WINPE"; mkdir -p "$WINPE"
xorriso -osirrox on -indev "$ISO" -extract / "$WINPE" >/dev/null 2>&1

echo "==> creating the FAT32 image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=1024 status=none

# The image gets a real MBR partition table.  A bare FAT32 volume is only
# seen as removable media by the firmware, which means no
# EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and therefore no way to touch the files.
echo "==> partitioning (MBR, one FAT32 partition)"
sfdisk --no-reread --no-tell-kernel "$IMG" <<'PART' >/dev/null
label: dos
unit: sectors
start=2048, size=2095104, type=c, bootable
PART

echo "==> creating the filesystem"
mkfs.vfat -F 32 -n ESP --offset 2048 "$IMG" >/dev/null

echo "==> copying the WinPE boot files"
# The 32 bit Windows boot manager.  It looks for the BCD next to itself and
# in the default location \EFI\Microsoft\Boot\BCD.
mmd -i "$IMG"@@1M ::/EFI
mmd -i "$IMG"@@1M ::/EFI/BOOT
mmd -i "$IMG"@@1M ::/EFI/Microsoft
mmd -i "$IMG"@@1M ::/EFI/Microsoft/BOOT
mmd -i "$IMG"@@1M ::/SOURCES
mmd -i "$IMG"@@1M ::/BOOT

mcopy -i "$IMG"@@1M "$WINPE/EFI/BOOT/BOOTIA32.EFI" ::/EFI/BOOT/BOOTIA32.EFI
mcopy -i "$IMG"@@1M "$WINPE/EFI/MICROSOF/BOOT/BCD"      ::/EFI/Microsoft/BOOT/BCD
mcopy -i "$IMG"@@1M "$WINPE/EFI/MICROSOF/BOOT/BCD"      ::/EFI/BOOT/BCD
mcopy -i "$IMG"@@1M "$WINPE/BOOT/BOOT.SDI"               ::/BOOT/BOOT.SDI
mcopy -i "$IMG"@@1M "$WINPE/BOOT/BOOTFIX.BIN"            ::/BOOT/BOOTFIX.BIN
mcopy -i "$IMG"@@1M "$WINPE/BOOT/BCD"                    ::/BOOT/BCD
echo "==> copying the WinPE image (this takes a moment)"
mcopy -i "$IMG"@@1M "$WINPE/SOURCES/BOOT.WIM"            ::/SOURCES/BOOT.WIM

echo "==> copying the 64 bit bootloader"
[ -f "$PROJECT/BOOTX64.EFI" ] && mcopy -i "$IMG"@@1M "$PROJECT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI

echo "==> ESP layout"
mdir -i "$IMG"@@1M -/ ::/EFI
mdir -i "$IMG"@@1M -/ ::/EFI/BOOT
mdir -i "$IMG"@@1M -/ ::/EFI/Microsoft/BOOT
mdir -i "$IMG"@@1M -/ ::/SOURCES
echo "image: $IMG"
