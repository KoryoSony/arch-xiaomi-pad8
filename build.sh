#!/usr/bin/env bash
# ./build.sh stock ROM.tgz | kernel | kmi | rootfs | initramfs | images | all ROM.tgz
# Run inside the build container (see Dockerfile).
set -euo pipefail
. "$(dirname "$0")/config.sh"

kmi() {
    KVER=$(cat "$OUT/kernel.release")
    python3 -I "$TOP/tools/check-kmi.py" "$OUT/Module.symvers" \
        "$WORK/kernel-stage/lib/modules/$KVER" "$STOCK/vendor_dlkm" "$STOCK/ramdisk"
}

step=${1:?usage: $0 stock ROM.tgz|kernel|kmi|rootfs|initramfs|images|all ROM.tgz}
case $step in
    stock)     "$TOP/tools/extract-stock.sh" "${2:?fastboot ROM path}" ;;
    kernel)    "$TOP/kernel/build-kernel.sh" ;;
    kmi)       kmi ;;
    rootfs)    "$TOP/rootfs/build-rootfs.sh" ;;
    initramfs) "$TOP/initramfs/build-initramfs.sh" ;;
    images)    "$TOP/tools/mkimages.sh" ;;
    all)
        "$TOP/tools/extract-stock.sh" "${2:?fastboot ROM path}"
        "$TOP/kernel/build-kernel.sh"
        kmi   # stops here if the stock vendor modules would not load
        "$TOP/rootfs/build-rootfs.sh"
        "$TOP/initramfs/build-initramfs.sh"
        "$TOP/tools/mkimages.sh"
        ;;
    *) echo "unknown step $step" >&2; exit 1 ;;
esac
