#!/usr/bin/env bash
# Puts the stock boot images and vbmeta back and formats userdata for Android.
# If anything else looks wrong, flash the full fastboot ROM with Xiaomi's
# flash_all.sh (it is the same package tools/extract-stock.sh used).
set -euo pipefail
. "$(dirname "$0")/config.sh"

[ "$(fastboot getvar product 2>&1 | sed -n 's/^product: *//p')" = "$DEVICE" ] || { echo "not a $DEVICE" >&2; exit 1; }
read -r -p "Restore Android (formats userdata)? Type RESTORE: " ok
[ "$ok" = RESTORE ] || exit 1
fastboot flash vbmeta "$STOCK_IMG/vbmeta.img"
fastboot flash boot "$STOCK_IMG/boot.img"
fastboot flash init_boot "$STOCK_IMG/init_boot.img"
fastboot -w
fastboot reboot
