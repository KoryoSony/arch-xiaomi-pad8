#!/usr/bin/env bash
# Builds the generic ramdisk that goes into init_boot.img. It only carries
# /init and a static aarch64 busybox (taken from the Arch rootfs); the vendor
# modules come from the stock vendor_boot ramdisk the bootloader loads first.
set -euo pipefail
. "$(dirname "$0")/../config.sh"

ROOTFS=$WORK/rootfs
bb=$ROOTFS/usr/bin/busybox
[ -f "$bb" ] || { echo "build the rootfs first (needs $bb)" >&2; exit 1; }
file -L "$bb" | grep -q 'statically linked' || { echo "$bb is not static" >&2; exit 1; }

d=$WORK/initramfs
rm -rf "$d"
mkdir -p "$d"/{bin,dev,proc,sys,run,newroot}
install -m755 "$bb" "$d/bin/busybox"
install -m755 "$TOP/initramfs/init" "$d/init"

(cd "$d" && find . | sort | cpio -o -H newc -R 0:0 --quiet | gzip -9n) > "$OUT/initramfs.cpio.gz"
echo "initramfs: $OUT/initramfs.cpio.gz"
