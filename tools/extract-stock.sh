#!/usr/bin/env bash
# Pulls everything the port reuses from a stock Xiaomi *fastboot* ROM
# (yupei_*_images_OS3.x.x.x.WPZ*.tgz):
#   - vendor_boot.img / dtbo.img / vbmeta*.img (flashed unchanged)
#   - stock boot.img + init_boot.img        (kept for restoring Android)
#   - vendor_dlkm + vendor_boot ramdisk modules (prebuilt vendor drivers)
#   - /vendor/firmware                       (GPU, touch, audio... firmware)
#
# usage: tools/extract-stock.sh path/to/fastboot-rom.tgz|extracted-dir
set -euo pipefail
. "$(dirname "$0")/../config.sh"

src=${1:?usage: $0 fastboot-rom.tgz|dir}
rm -rf "$STOCK" "$STOCK_IMG" && mkdir -p "$STOCK"/{vendor_dlkm,vendor,ramdisk,unpacked} "$STOCK_IMG"
tmp=$WORK/stock-tmp; rm -rf "$tmp"; mkdir -p "$tmp"

if [ -f "$src" ]; then
    # Only what we use: skips userdata.img, firmware blobs etc. (disk is tight).
    tar -xzf "$src" -C "$tmp" --wildcards \
        '*/images/boot.img' '*/images/init_boot.img' '*/images/vendor_boot.img' \
        '*/images/dtbo.img' '*/images/vbmeta*.img' '*/images/super.img*'
    src=$tmp
fi
images=$(dirname "$(find "$src" -name vendor_boot.img -print -quit)")
[ -f "$images/vendor_boot.img" ] || { echo "vendor_boot.img not found in $src" >&2; exit 1; }

for i in boot init_boot vendor_boot dtbo vbmeta vbmeta_system; do
    if [ -f "$images/$i.img" ]; then cp "$images/$i.img" "$STOCK_IMG/"; fi
done

kver=$(grep -aom1 'Linux version [^ ]* ' "$STOCK_IMG/boot.img" || true)
echo "== stock kernel: ${kver:-unknown (compressed Image)}"
echo "   configured source: $KERNEL_REPO @ $KERNEL_BRANCH"
echo "== stock boot image headers (mkimages.sh reuses os_version and patch level)"
unpack_bootimg --boot_img "$STOCK_IMG/boot.img" --out "$STOCK/unpacked/boot" --format mkbootimg | tee "$STOCK/boot.args"
echo
unpack_bootimg --boot_img "$STOCK_IMG/vendor_boot.img" --out "$STOCK/unpacked/vendor_boot" --format mkbootimg | tee "$STOCK/vendor_boot.args"
echo

# First-stage modules live in the vendor ramdisk(s).
decompress() {
    case $(od -An -tx1 -N4 "$1" | tr -d ' \n') in
        1f8b*)    gzip -dc "$1" ;;
        28b52ffd) zstd -dc "$1" ;;
        02214c18|04224d18) lz4 -dc "$1" ;;
        *)        cat "$1" ;;
    esac
}
for r in "$STOCK"/unpacked/vendor_boot/vendor_ramdisk*; do
    decompress "$r" | (cd "$STOCK/ramdisk" && cpio -idmu --quiet)
done

# super.img may be split (super.img.0, .1, ...) and is sparse.
supers=$(ls "$images"/super.img* 2>/dev/null | sort -V | paste -sd, -)
[ -n "$supers" ] || { echo "super.img not found" >&2; exit 1; }
if [ "$(od -An -tx1 -N4 "${supers%%,*}" | tr -d ' \n')" = 3aff26ed ]; then
    simg2img "$supers" "$tmp/super.raw"
    [ "$src" = "$tmp" ] && rm -f "$images"/super.img*
else
    if [ "$src" = "$tmp" ]; then mv "${supers%%,*}" "$tmp/super.raw"; else cp "${supers%%,*}" "$tmp/super.raw"; fi
fi
lpdump "$tmp/super.raw" | grep -E '^\s+Name:' | sort -u || true

extract_fs() {   # extract_fs <lp-partition> <dest>
    local p=$1 dest=$2
    lpunpack -p "$p" "$tmp/super.raw" "$tmp" >/dev/null
    local img=$tmp/$p.img
    # EROFS superblock magic 0xE0F5E1E2 at offset 1024; anything else is ext4.
    if [ "$(od -An -tx1 -j1024 -N4 "$img" | tr -d ' \n')" = e2e1f5e0 ]; then
        fsck.erofs --extract="$dest" "$img"
    else
        debugfs -R "rdump / $dest" "$img" 2>/dev/null
    fi
    rm -f "$img"
}
extract_fs vendor_dlkm_a "$STOCK/vendor_dlkm"
extract_fs vendor_a "$STOCK/vendor"
extract_fs odm_a "$STOCK/odm"         # touch (NT36532) + camera/sensor firmware

rm -rf "$tmp"

# The published source is a $KERNEL_SERIES kernel; a ROM whose vendor modules
# were built for another series can never be matched.
ko=$(find "$STOCK/vendor_dlkm" -name '*.ko' -print -quit)
magic=$(modinfo -F vermagic "$ko")
echo "stock vendor modules: vermagic '$magic'"
case $magic in
    "$KERNEL_SERIES".*) ;;
    *) echo "ERROR: this ROM's kernel is not $KERNEL_SERIES.x; Xiaomi's published source ($KERNEL_BRANCH) cannot work with it." >&2
       echo "Use a fastboot ROM whose kernel is $KERNEL_SERIES (HyperOS 3 / Android 16) and flash it to the tablet first." >&2
       exit 1 ;;
esac
nmods=$(find "$STOCK/vendor_dlkm" "$STOCK/ramdisk" -name '*.ko' | wc -l)
echo "extracted $nmods stock modules and $(du -sh "$STOCK/vendor/firmware" | cut -f1) of vendor firmware into $STOCK"
