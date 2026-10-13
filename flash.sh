#!/usr/bin/env bash
# Flashes Arch to the CURRENT slot of a Xiaomi Pad 8 in fastboot mode.
# Erases userdata (all Android data). Undo with ./restore-android.sh.
#
#   ./flash.sh          flash boot, init_boot, vbmeta (verification off), userdata
#   ./flash.sh --test   RAM-boot only (fastboot boot), nothing is written
set -euo pipefail
. "$(dirname "$0")/config.sh"

fb() { fastboot "$@"; }
var() { fastboot getvar "$1" 2>&1 | sed -n "s/^$1: *//p" | head -n1; }

product=$(var product)
[ "$product" = "$DEVICE" ] || { echo "connected device is '$product', expected '$DEVICE'" >&2; exit 1; }
[ "$(var unlocked)" = yes ] || { echo "bootloader is locked; unlock it first (Mi Unlock)" >&2; exit 1; }
slot=$(var current-slot)

if [ "${1:-}" = --test ]; then
    [ -f "$OUT/boot-test.img" ] || { echo "missing $OUT/boot-test.img" >&2; exit 1; }
    echo "RAM-booting; if it comes up, connect USB and 'telnet 172.16.42.1'."
    fb boot "$OUT/boot-test.img"
    exit 0
fi

for f in boot.img init_boot.img userdata.img; do [ -f "$OUT/$f" ] || { echo "missing $OUT/$f" >&2; exit 1; }; done
[ -f "$STOCK/img/vbmeta.img" ] || { echo "missing $STOCK/img/vbmeta.img (run tools/extract-stock.sh)" >&2; exit 1; }

cat <<MSG
About to flash Arch Linux to slot $slot of this $product:
  boot$slot, init_boot$slot  <- Arch kernel / initramfs
  vbmeta$slot                <- stock, with AVB verification disabled
  userdata                   <- Arch root filesystem (ALL ANDROID DATA IS ERASED)
MSG
read -r -p "Type ERASE to continue: " ok
[ "$ok" = ERASE ] || { echo "aborted"; exit 1; }

fb --disable-verity --disable-verification flash vbmeta "$STOCK/img/vbmeta.img"
fb flash boot "$OUT/boot.img"
fb flash init_boot "$OUT/init_boot.img"
fb flash userdata "$OUT/userdata.img"
fb reboot
echo "Booting. Connect USB and run: ssh alarm@172.16.42.1 (password alarm)"
