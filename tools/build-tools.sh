#!/usr/bin/env bash
# Builds the on-device helpers into out/: kmstest, kmsatomic, kmsprops,
# yupei-bootlog (static), yupei-present (sway) and yupei-present-gnome.
# Run inside the build container after the kernel build (needs its font file).
set -euo pipefail
. "$(dirname "$0")/../config.sh"
pacman -S --noconfirm --needed wayland wlr-protocols glib2 glib2-devel libpipewire pkgconf >/dev/null
T=$TOP/tools G=$(mktemp -d)
python3 -I "$T/gen-font.py" "$WORK/kernel/lib/fonts/font_8x16.c" "$G/font8x16.h"
X=$(find /usr/share/wlr-protocols -name wlr-screencopy-unstable-v1.xml)
wayland-scanner client-header "$X" "$G/wlr-screencopy-unstable-v1-client-protocol.h"
wayland-scanner private-code "$X" "$G/wlr-screencopy-protocol.c"
for t in kmstest kmsatomic kmsprops thp-capture; do gcc -O2 -Wall -static -o "$OUT/$t" "$T/$t.c"; done
gcc -O2 -Wall -static -o "$OUT/yupei-touchd" "$T/yupei-touchd.c" -lm
gcc -O2 -Wall -static -o "$OUT/yupei-powerkey" "$T/yupei-powerkey.c"
gcc -O2 -Wall -static -I"$G" -o "$OUT/yupei-bootlog" "$T/yupei-bootlog.c"
gcc -O2 -Wall -I"$G" -o "$OUT/yupei-present" "$T/yupei-present.c" "$G/wlr-screencopy-protocol.c" -lwayland-client
gcc -O2 -Wall -o "$OUT/yupei-present-gnome" "$T/yupei-present-gnome.c" \
    $(pkg-config --cflags --libs gio-2.0 libpipewire-0.3) -lpthread
rm -rf "$G"
echo "tools built in $OUT"
