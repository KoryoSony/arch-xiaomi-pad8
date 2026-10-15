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
# Static boot splash / log viewer (tools/build-tools.sh), optional.
[ -x "$OUT/yupei-bootlog" ] && install -m755 "$OUT/yupei-bootlog" "$d/bin/yupei-bootlog"
# Static status LED animator (tools/build-tools.sh), optional.
[ -x "$OUT/yupei-led" ] && install -m755 "$OUT/yupei-led" "$d/bin/yupei-led"

# Legacy LZ4 like Android's ramdisks: the bootloader appends ours to the
# lz4-legacy vendor ramdisk, and the kernel's unlz4 reads concatenated
# archives chunk by chunk, so a gzip segment after it is never unpacked.
(cd "$d" && find . | sort | cpio -o -H newc -R 0:0 --quiet) | lz4 -l -12 --favor-decSpeed -f - "$OUT/initramfs.cpio.lz4"
echo "initramfs: $OUT/initramfs.cpio.lz4"
