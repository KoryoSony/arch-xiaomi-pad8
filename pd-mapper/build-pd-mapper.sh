#!/usr/bin/env bash
# Builds linux-msm's pd-mapper (QMI service locator): the ADSP's audio
# protection domain stays down ("q6 state is down") until the apps side
# answers its service-locator queries. Android runs its own pd-mapper.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
pacman -S --noconfirm --needed meson ninja >/dev/null
cd "$WORK"
rm -rf qrtr pd-mapper
git clone --depth 1 https://github.com/linux-msm/qrtr
git clone --depth 1 https://github.com/linux-msm/pd-mapper
meson setup qrtr/build qrtr -Dprefix=/usr -Ddefault_library=static
ninja -C qrtr/build
DESTDIR=$WORK/qrtr-dest ninja -C qrtr/build install >/dev/null
cd pd-mapper
# The firmware path parameter may lack a trailing newline (we write it without
# one); upstream always drops the last character ("image" -> "imag").
sed -i 's|^\tfw_sysfs_path\[n - 1\] = .\\0.;|\tfw_sysfs_path[n] = 0;\n\tif (fw_sysfs_path[n - 1] == 0x0a)\n\t\tfw_sysfs_path[n - 1] = 0;|' pd-mapper.c
grep -q "fw_sysfs_path\[n\] = 0" pd-mapper.c
gcc -O2 -Wall -I"$WORK/qrtr-dest/usr/include" -o pd-mapper *.c \
    "$WORK/qrtr-dest/usr/lib/libqrtr.a" -llzma
install -Dm755 pd-mapper "$OUT/pd-mapper"
file "$OUT/pd-mapper"
