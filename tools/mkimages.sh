#!/usr/bin/env bash
# Packs boot.img (our kernel) and init_boot.img (our ramdisk) as header v4
# images, reusing the stock OS version / patch level. vendor_boot, dtbo and
# vbmeta stay stock: they carry the device tree and first-stage modules.
set -euo pipefail
. "$(dirname "$0")/../config.sh"

arg() { grep -o -- "--$1 [^ ]*" "$STOCK/boot.args" | head -n1 | cut -d' ' -f2 | tr -d "'"; }
hdr=$(arg header_version); osv=$(arg os_version); ospl=$(arg os_patch_level)
[ "$hdr" = 4 ] || { echo "stock boot.img is header v$hdr; this script only handles v4" >&2; exit 1; }

# Appended to the vendor_boot cmdline by the bootloader.
CMDLINE="firmware_class.path=/vendor/firmware_mnt/image printk.devkmsg=on loglevel=7 ${EXTRA_CMDLINE:-}"

mkbootimg --header_version 4 --os_version "$osv" --os_patch_level "$ospl" \
    --kernel "$OUT/Image" --cmdline "$CMDLINE" -o "$OUT/boot.img"
mkbootimg --header_version 4 --os_version "$osv" --os_patch_level "$ospl" \
    --ramdisk "$OUT/initramfs.cpio.gz" -o "$OUT/init_boot.img"

# Kernel + ramdisk in one image for "./flash.sh --test" (fastboot boot, RAM only).
# yupei.debug brings up the USB telnet shell and waits before switching root.
mkbootimg --header_version 4 --os_version "$osv" --os_patch_level "$ospl" \
    --kernel "$OUT/Image" --ramdisk "$OUT/initramfs.cpio.gz" \
    --cmdline "yupei.debug $CMDLINE" -o "$OUT/boot-test.img"

for i in boot init_boot; do
    stock=$(stat -c %s "$STOCK_IMG/$i.img")
    ours=$(stat -c %s "$OUT/$i.img")
    [ "$ours" -le "$stock" ] || { echo "$i.img ($ours bytes) is larger than the stock partition image ($stock)" >&2; exit 1; }
done
echo "images: $OUT/boot.img $OUT/init_boot.img (os $osv, patch $ospl)"
