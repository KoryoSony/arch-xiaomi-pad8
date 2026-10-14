#!/usr/bin/env bash
# Builds Arch Linux ARM's ffmpeg with the V4L2 M2M dma-buf/DRM PRIME patch
# (hardware video decoding on Qualcomm msm_vidc: H.264, HEVC, VP9) into
# out/ffmpeg-*.pkg.tar.*. Run inside the arm64 build container.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
id builder >/dev/null 2>&1 || useradd -m builder
echo 'builder ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/builder
B=$WORK/ffmpeg-build
rm -rf "$B" && mkdir -p "$B"
cp "$TOP/ffmpeg/PKGBUILD" "$TOP/ffmpeg/"*.patch "$B/"
chown -R builder "$B"
cd "$B"
sudo -u builder makepkg -s -A --noconfirm --nocheck --skippgpcheck
cp "$B"/ffmpeg-*.pkg.tar.* "$OUT/"
ls -la "$OUT"/ffmpeg-*.pkg.tar.*
