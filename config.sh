# Shared settings for every build step. Sourced, not executed.

DEVICE=yupei                       # Xiaomi Pad 8 (P82)
KERNEL_REPO=https://github.com/MiCode/Xiaomi_Kernel_OpenSource
KERNEL_BRANCH=yupei-w-oss          # Linux 6.6 GKI + Qualcomm "sun" platform, HyperOS 3 / Android 16
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
