#!/usr/bin/env bash
# Builds Arch's mutter with the split-primary-plane patch (native GNOME on the
# yupei panel) into out/mutter-*.pkg.tar.*. Run inside the arm64 build container.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
id builder >/dev/null 2>&1 || useradd -m builder
echo 'builder ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/builder
B=$WORK/mutter-build
rm -rf "$B" && mkdir -p "$B"
cp "$TOP/mutter/PKGBUILD" "$TOP/mutter/"*.patch "$B/"
chown -R builder "$B"
cd "$B"
sudo -u builder makepkg -s --noconfirm --nocheck --skippgpcheck
cp "$B"/mutter-*.pkg.tar.* "$OUT/"
ls -la "$OUT"/mutter-*.pkg.tar.*
