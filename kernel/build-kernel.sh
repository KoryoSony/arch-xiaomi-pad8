#!/usr/bin/env bash
# Builds the GKI kernel Image + GKI (system_dlkm) modules from Xiaomi's tree
# with the Linux-userspace fragment applied. Vendor modules are NOT rebuilt:
# parts of their source were never published (see docs/PORTING.md), so the
# stock prebuilts from the device firmware are used instead.
set -euo pipefail
. "$(dirname "$0")/../config.sh"

SRC=$WORK/kernel
KOUT=$WORK/kernel-out
mkdir -p "$WORK" "$OUT"

if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$KERNEL_BRANCH" "$KERNEL_REPO" "$SRC"
fi

# Xiaomi's release points at directories that were not published. Replace the
# dangling symlinks / missing Kconfig with empty stubs so the tree configures.
# Nothing in gki_defconfig depends on them.
stub_dir() {
    local d=$SRC/$1
    grep -qs "source \"$1/Kconfig\"" "$(dirname "$d")/Kconfig" || return 0
    if [ -L "$d" ] && [ ! -e "$d" ]; then rm "$d"; fi
    mkdir -p "$d"
    [ -e "$d/Kconfig" ] || : > "$d/Kconfig"
    [ -e "$d/Makefile" ] || : > "$d/Makefile"
}
stub_dir drivers/misc/hwid
stub_dir drivers/power/supply/mca
for l in include/linux/mca arch/arm64/boot/dts/vendor Documentation/devicetree/bindings; do
    if [ -L "$SRC/$l" ] && [ ! -e "$SRC/$l" ]; then rm "$SRC/$l"; mkdir -p "$SRC/$l"; fi
done

# Our kernel patches (kernel/patches): Bluetooth on the WCN7750 over a plain
# UART attach. Applied once; skipped when already in the tree.
for p in "$TOP"/kernel/patches/*.patch; do
    if git -C "$SRC" apply --check "$p" 2>/dev/null; then
        git -C "$SRC" apply "$p"
    elif ! git -C "$SRC" apply --check -R "$p" 2>/dev/null; then
        echo "kernel patch does not apply: $p" >&2; exit 1
    fi
done

# Toolchain: Google's pinned clang on x86_64 hosts. Elsewhere there is no
# prebuilt, so use the distro's clang 18 (LLVM_DIR, set by the arm64 image),
# the same major version r510928 is based on.
if [ "$(uname -m)" = x86_64 ] && [ "${USE_SYSTEM_CLANG:-0}" != 1 ]; then
    TC=$WORK/clang-$CLANG_VERSION
    if [ ! -x "$TC/bin/clang" ]; then
        mkdir -p "$TC"
        curl -fL "$CLANG_URL" | tar -xz -C "$TC"
    fi
    export PATH=$TC/bin:$PATH
elif [ -n "${LLVM_DIR:-}" ]; then
    export PATH=$LLVM_DIR:$PATH
fi
echo "using $(command -v clang): $(clang --version | head -1)"

# Host-only flag: glibc >= 2.42 makes strchr() const-correct, which libbpf
# (built for resolve_btfids) turns into an error. Kernel objects are unaffected.
MAKE=(make -C "$SRC" O="$KOUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 -j"$(nproc)"
      HOSTCFLAGS="-Wno-error=incompatible-pointer-types-discards-qualifiers")

"${MAKE[@]}" gki_defconfig
"$SRC/scripts/kconfig/merge_config.sh" -m -O "$KOUT" "$KOUT/.config" "$TOP/kernel/yupei-linux.fragment"
"${MAKE[@]}" olddefconfig

# merge_config silently drops options whose dependencies are unmet; fail instead.
while IFS= read -r line; do
    case $line in CONFIG_*=*) ;; *) continue ;; esac
    grep -qxF "$line" "$KOUT/.config" || { echo "fragment option not applied: $line" >&2; exit 1; }
done < "$TOP/kernel/yupei-linux.fragment"

# Escape hatch if a newer compiler warns where clang 18 did not; warnings do
# not affect the KMI, which check-kmi.py verifies regardless.
if [ "${KERNEL_NO_WERROR:-0}" = 1 ]; then
    "$SRC/scripts/config" --file "$KOUT/.config" --disable WERROR
    "${MAKE[@]}" olddefconfig
fi

"${MAKE[@]}" Image modules

KVER=$(cat "$KOUT/include/config/kernel.release")
STAGE=$WORK/kernel-stage
rm -rf "$STAGE"
"${MAKE[@]}" INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 modules_install
rm -f "$STAGE/lib/modules/$KVER/build" "$STAGE/lib/modules/$KVER/source"

cp "$KOUT/arch/arm64/boot/Image" "$OUT/Image"
cp "$KOUT/Module.symvers" "$OUT/Module.symvers"
cp "$KOUT/.config" "$OUT/kernel.config"
echo "$KVER" > "$OUT/kernel.release"
echo "kernel $KVER built: $OUT/Image"
