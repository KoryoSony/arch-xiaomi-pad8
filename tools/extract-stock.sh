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
rm -rf "$STOCK" && mkdir -p "$STOCK"/{img,vendor_dlkm,vendor,ramdisk,unpacked}
tmp=$WORK/stock-tmp; rm -rf "$tmp"; mkdir -p "$tmp"

if [ -f "$src" ]; then
    tar -xzf "$src" -C "$tmp"
    src=$tmp
fi
images=$(dirname "$(find "$src" -name vendor_boot.img -print -quit)")
[ -f "$images/vendor_boot.img" ] || { echo "vendor_boot.img not found in $src" >&2; exit 1; }

for i in boot init_boot vendor_boot dtbo vbmeta vbmeta_system; do
    [ -f "$images/$i.img" ] && cp "$images/$i.img" "$STOCK/img/"
done

echo "== stock boot image headers (mkimages.sh reuses os_version and patch level)"
unpack_bootimg --boot_img "$STOCK/img/boot.img" --out "$STOCK/unpacked/boot" --format mkbootimg | tee "$STOCK/boot.args"
echo
unpack_bootimg --boot_img "$STOCK/img/vendor_boot.img" --out "$STOCK/unpacked/vendor_boot" --format mkbootimg | tee "$STOCK/vendor_boot.args"
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
simg2img "$supers" "$tmp/super.raw"
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

rm -rf "$tmp"
nmods=$(find "$STOCK/vendor_dlkm" "$STOCK/ramdisk" -name '*.ko' | wc -l)
echo "extracted $nmods stock modules and $(du -sh "$STOCK/vendor/firmware" | cut -f1) of vendor firmware into $STOCK"
