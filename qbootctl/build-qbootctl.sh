#!/usr/bin/env bash
# Builds qbootctl (linux-msm), the Qualcomm A/B boot control tool: marks the
# running slot successful so the bootloader doesn't count every Arch boot as
# a failed attempt (7 of them => slot unbootable => stuck in fastboot).
set -euo pipefail
. "$(dirname "$0")/../config.sh"
pacman -S --noconfirm --needed meson ninja zlib >/dev/null
rm -rf "$WORK/qbootctl"
git clone --depth 1 https://github.com/linux-msm/qbootctl "$WORK/qbootctl"
cd "$WORK/qbootctl"
meson setup build --buildtype=release
ninja -C build
install -Dm755 build/qbootctl "$OUT/qbootctl"
file "$OUT/qbootctl"
