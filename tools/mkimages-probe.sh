#!/usr/bin/env bash
# Builds out/init_boot-Pn.img: the real initramfs with PROBE=Pn baked in.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
P=${1:?usage: $0 P1..P6}
d=$WORK/initramfs-$P
rm -rf "$d"; cp -a "$WORK/initramfs" "$d"
cp "$TOP/initramfs/init" "$d/init"
sed -i "s/^PROBE=\$/PROBE=$P/" "$d/init"
grep -q "^PROBE=$P\$" "$d/init"
(cd "$d" && find . | sort | cpio -o -H newc -R 0:0 --quiet) | lz4 -l -12 -f - "$WORK/initramfs-$P.lz4" >/dev/null
mkbootimg --header_version 4 --ramdisk "$WORK/initramfs-$P.lz4" -o "$OUT/init_boot-$P.img"
echo "$OUT/init_boot-$P.img"
