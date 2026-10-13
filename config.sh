# Shared settings for every build step. Sourced, not executed.

DEVICE=yupei                       # Xiaomi Pad 8 (P82)
# The tablet's boot.img runs Google's GKI, not a Xiaomi build: HyperOS
# OS4.0.5.0 ships "6.6.118-android15-8-gc4127a25dcf3-ab15863337", which is
# commit c4127a25dcf3 = tag android15-6.6-2026-01_r37. Building from that exact
# tag gives identical symbol CRCs for the prebuilt vendor modules. When the ROM
# changes, tools/extract-stock.sh prints the new version; find its tag with
#   git ls-remote https://android.googlesource.com/kernel/common | grep ^<sha>
KERNEL_REPO=https://android.googlesource.com/kernel/common
KERNEL_BRANCH=android15-6.6-2026-01_r37
# Xiaomi's tree (MiCode/Xiaomi_Kernel_OpenSource yupei-w-oss, 6.6.57) is the same
# KMI generation but older, and only matters for the vendor modules we reuse.
KERNEL_SERIES=6.6                  # the stock ROM's vendor modules must be built for this series

# Google's kernel toolchain pinned by build.config.constants (CLANG_VERSION=r510928).
CLANG_VERSION=r510928
CLANG_URL=https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-${CLANG_VERSION}.tar.gz

ALARM_ROOTFS_URL=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
ROOTFS_IMAGE_SIZE=8G               # grown to fill userdata on first boot

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${WORK:-$TOP/work}
OUT=${OUT:-$TOP/out}
STOCK=${STOCK:-$WORK/stock}        # filled by tools/extract-stock.sh
STOCK_IMG=$OUT/stock               # stock partition images, needed on the flashing host
