#!/usr/bin/env bash
# Builds Mesa with Turnip (Vulkan) on Qualcomm's KGSL kernel driver and Zink
# (OpenGL over Vulkan) into /opt/mesa-yupei, packed as out/mesa-yupei.tar.zst.
# msm is built too: with kgsl alone Mesa drops DRM support, and GBM needs it.
# glvnd: EGL is installed as a vendor library (libEGL_mesa.so.0) behind the
# system's glvnd libEGL/libGL. Without it, programs that use desktop GL through
# glvnd's libGL (Firefox) got no GL at all next to our standalone libEGL:
# its contexts failed their init checks and WebRender fell back to software.
# Run inside the arm64 build container.
set -euo pipefail
. "$(dirname "$0")/../config.sh"
MESA_REF=${MESA_REF:-main}
SRC=$WORK/mesa
PREFIX=/opt/mesa-yupei
pacman -S --noconfirm --needed meson ninja python-mako python-packaging python-yaml \
    glslang libdrm wayland wayland-protocols libelf zstd expat bison flex \
    libx11 libxext libxfixes libxshmfence libxxf86vm libxrandr spirv-tools \
    python-ply libglvnd >/dev/null
if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 -b "$MESA_REF" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
fi
python3 -I "$TOP/mesa/0001-freedreno-add-Adreno-825.patch.py" "$SRC/src/freedreno/common/freedreno_devices.py"
meson setup --wipe "$WORK/mesa-build" "$SRC" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release \
    -Dplatforms=wayland -Dglx=disabled -Dglvnd=enabled -Degl=enabled -Dgbm=enabled -Dgles1=disabled \
    -Dgallium-drivers=zink -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm,kgsl \
    -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false \
    -Dvideo-codecs= -Dtools=
ninja -C "$WORK/mesa-build"
rm -rf "$WORK/mesa-dest"
DESTDIR="$WORK/mesa-dest" ninja -C "$WORK/mesa-build" install
tar -C "$WORK/mesa-dest" -cf - opt | zstd -q -f -o "$OUT/mesa-yupei.tar.zst"
git -C "$SRC" log -1 --format='mesa %h %cd' > "$OUT/mesa-yupei.version"
echo "mesa: $OUT/mesa-yupei.tar.zst ($(cat "$OUT/mesa-yupei.version"))"
