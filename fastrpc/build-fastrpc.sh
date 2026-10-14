#!/usr/bin/env bash
# Builds Qualcomm's open-source FastRPC userspace (quic/fastrpc): libadsprpc
# and adsprpcd. Android starts "audioadsprpcd audiopd adsp" before the ADSP's
# audio PD; without that listener the audio framework (SPF/APM) never answers.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
pacman -S --noconfirm --needed autoconf automake libtool pkgconf libyaml libbsd >/dev/null
cd "$WORK"
rm -rf fastrpc fastrpc-dest
git clone --depth 1 https://github.com/quic/fastrpc
cd fastrpc
./gitcompile --prefix=/usr/local >/dev/null 2>&1 || { autoreconf -fi && ./configure --prefix=/usr/local && make -j"$(nproc)"; }
make DESTDIR="$WORK/fastrpc-dest" install >/dev/null
tar -C "$WORK/fastrpc-dest" -czf "$OUT/fastrpc.tar.gz" .
ls "$WORK/fastrpc-dest/usr/local/bin" "$WORK/fastrpc-dest/usr/local/lib"
