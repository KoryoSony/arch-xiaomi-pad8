#!/usr/bin/env bash
# Builds the Arch Linux ARM root filesystem image for the userdata partition.
# Needs root (run inside the --privileged build container).
#
#   DESKTOP=none   headless: SSH over USB / Wi-Fi (default, recommended first boot)
#   DESKTOP=sway   + sway with the pixman software renderer
set -euo pipefail
. "$(dirname "$0")/../config.sh"

DESKTOP=${DESKTOP:-none}
ROOTFS=$WORK/rootfs
KVER=$(cat "$OUT/kernel.release")
KSTAGE=$WORK/kernel-stage/lib/modules/$KVER
[ -d "$KSTAGE" ] || { echo "build the kernel first" >&2; exit 1; }
[ -d "$STOCK/vendor_dlkm" ] || { echo "run tools/extract-stock.sh first" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }

# Let x86_64 hosts run aarch64 binaries in the chroot.
if [ "$(uname -m)" != aarch64 ] && [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
    cut -d: -f2- /usr/lib/binfmt.d/qemu-aarch64-static.conf | sed 's/^/:/' > /proc/sys/fs/binfmt_misc/register
fi

# Reuse the checksum-verified tarball docker/build-image.sh already fetched.
tarball=$TOP/docker/alarm-base.tar.gz
if [ ! -f "$tarball" ]; then
    tarball=$WORK/$(basename "$ALARM_ROOTFS_URL")
    [ -f "$tarball" ] || curl -fL -o "$tarball" "$ALARM_ROOTFS_URL"
fi
rm -rf "$ROOTFS" && mkdir -p "$ROOTFS"
bsdtar -xpf "$tarball" -C "$ROOTFS"

# Overlay first: the chroot step enables units it ships.
cp -a "$TOP/rootfs/overlay/." "$ROOTFS/"

packages=(busybox networkmanager openssh sudo vim htop usbutils pciutils
          iw wireless-regdb bluez bluez-utils libinput evtest)
case $DESKTOP in
    none) ;;
    sway) packages+=(sway foot wofi seatd) ;;
    *) echo "unknown DESKTOP=$DESKTOP" >&2; exit 1 ;;
esac

arch-chroot "$ROOTFS" /bin/bash -euxc "
    # pacman's disk-space check and download sandbox cannot see real mounts
    # inside a container chroot; disable them for the build, restore after.
    cp /etc/pacman.conf /etc/pacman.conf.orig
    sed -i -e 's/^CheckSpace/#CheckSpace/' -e '/^\[options\]/a DisableSandbox' /etc/pacman.conf
    pacman-key --init
    pacman-key --populate archlinuxarm
    pacman -Rdd --noconfirm linux-aarch64 || true
    pacman -Syu --noconfirm --needed ${packages[*]}
    echo xiaomi-pad8 > /etc/hostname
    ln -sf /usr/share/zoneinfo/UTC /etc/localtime
    systemctl enable NetworkManager sshd systemd-networkd yupei-firmware yupei-usb-gadget yupei-usb-role yupei-cpuinfo yupei-desktop yupei-touchd
    dconf update 2>/dev/null || true
    systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target serial-getty@hvc0.service
    pacman -Scc --noconfirm
    mv /etc/pacman.conf.orig /etc/pacman.conf
    # The GKI kernel has no Landlock, so pacman's download sandbox always fails.
    sed -i '/^\\[options\\]/a DisableSandbox' /etc/pacman.conf
"


# Kernel: our GKI modules + stock vendor modules (first stage and vendor_dlkm).
m=$ROOTFS/usr/lib/modules/$KVER
rm -rf "$m" && mkdir -p "$m/vendor" "$m/vendor_boot"
cp -a "$KSTAGE/." "$m/"
find "$STOCK/vendor_dlkm" -name '*.ko' -exec cp {} "$m/vendor/" \;
find "$STOCK/ramdisk/lib/modules" -name '*.ko' -exec cp {} "$m/vendor_boot/" \;
depmod -C "$ROOTFS/etc/depmod.d" -b "$ROOTFS/usr" "$KVER"

# Load vendor_dlkm modules in Android's order, minus its blocklist.
mkdir -p "$ROOTFS/etc/modules-load.d"
load=$(find "$STOCK/vendor_dlkm" -name modules.load | head -n1)
block=$(find "$STOCK/vendor_dlkm" -name modules.blocklist | head -n1)
{
    echo "# Generated from the stock vendor_dlkm modules.load"
    while read -r ko; do
        name=$(basename "$ko" .ko)
        [ -n "$block" ] && grep -qE "^blocklist +$name\$" "$block" && continue
        echo "$name"
    done < "$load"
} > "$ROOTFS/etc/modules-load.d/yupei-vendor.conf"

# Vendor firmware (GPU, touch, audio, Wi-Fi config...). The firmware
# partitions (modem, dsp, bluetooth) are mounted at boot instead.
mkdir -p "$ROOTFS/usr/lib/firmware"
cp -a "$STOCK/vendor/firmware/." "$ROOTFS/usr/lib/firmware/"
# odm firmware: the touch controller keeps its firmware in RAM and the driver
# reloads novatek_nt36532_yupei_fw_*.bin from here on every display resume.
cp -a "$STOCK/odm/firmware/." "$ROOTFS/usr/lib/firmware/"
mkdir -p "$ROOTFS"/vendor/{firmware_mnt,dsp,bt_firmware}

# Pack as a sparse ext4 image for "fastboot flash userdata".
raw=$WORK/rootfs.img
rm -f "$raw"
mkfs.ext4 -F -L arch -d "$ROOTFS" "$raw" "$ROOTFS_IMAGE_SIZE"
img2simg "$raw" "$OUT/userdata.img"
rm -f "$raw"
echo "rootfs: $OUT/userdata.img (default login alarm/alarm, root/root: change them)"
