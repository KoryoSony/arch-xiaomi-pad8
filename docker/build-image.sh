#!/usr/bin/env bash
# Builds the native arm64 image from the official Arch Linux ARM tarball
# (checksum-verified), so no third-party base image is involved.
set -euo pipefail
cd "$(dirname "$0")"
url=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
if [ ! -f alarm-base.tar.gz ]; then
    curl -fL -o alarm-base.tar.gz.part "$url"
    want=$(curl -fsL "$url.md5" | cut -d' ' -f1)
    have=$( (md5sum alarm-base.tar.gz.part 2>/dev/null || md5 -r alarm-base.tar.gz.part) | cut -d' ' -f1)
    [ "$want" = "$have" ] || { echo "checksum mismatch: $have != $want" >&2; exit 1; }
    mv alarm-base.tar.gz.part alarm-base.tar.gz
fi
docker build --platform linux/arm64 -f Dockerfile.arm64 -t yupei-build-arm64 .
