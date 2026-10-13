# Porting notes: Xiaomi Pad 8 (`yupei`)

## What Xiaomi published

| Repo (MiCode, branch `yupei-w-oss`) | Contents | Used here |
|---|---|---|
| `Xiaomi_Kernel_OpenSource` | Linux 6.6.57 Android GKI tree + Qualcomm `sun` platform (`yupei.bzl` → `sun.bzl`) | Builds our kernel `Image` and GKI modules |
| `kernel_devicetree` | `sun`/SM87xx device trees | Not needed: the stock DTB in `vendor_boot` is reused |
| `vendor_opensource_display-drivers`, `vendor_qcom_opensource_{audio,camera,data}-kernel`, `vendor_qcom_opensource_wlan`, `vendor_xiaomi_proprietary_touch-driver`, `*-devicetree` | Out-of-tree vendor modules | Not rebuilt; reference for debugging |

**There is no GPU (KGSL) kernel source**, and the kernel tree itself is incomplete:

- `drivers/misc/hwid/` (`CONFIG_MI_HARDWARE_ID`): not published, but sourced by `drivers/misc/Kconfig`.
- `drivers/power/supply/mca` and `include/linux/mca` are dangling symlinks into
  `vendor/xiaomi/proprietary/mca` (Xiaomi's charging stack, `CONFIG_XM_POWER_SUPPLY`).
  `drivers/usb/dwc3/dwc3-msm-core.c` includes those headers.
- `arch/arm64/boot/dts/vendor` is a symlink to a separate checkout.

So the vendor module set **cannot** be rebuilt from source. `kernel/build-kernel.sh`
stubs these paths so that `gki_defconfig` configures, which is all we need.

## Boot chain (header v4, A/B)

```
ABL ── boot.img        ← ours: kernel Image + cmdline
   ├── init_boot.img   ← ours: ramdisk with /init (busybox) → switch_root
   ├── vendor_boot.img ← stock: DTB, vendor ramdisk (first-stage .ko + modules.load), bootconfig
   ├── dtbo.img        ← stock
   └── vbmeta.img      ← stock, flashed with verification disabled
userdata               ← ext4 Arch rootfs (fastboot-flashed sparse image)
super (vendor_dlkm, vendor) ← untouched; contents copied into the rootfs at build time
modem / dsp / bluetooth     ← mounted read-only at boot for remoteproc firmware
```

The bootloader concatenates the vendor ramdisk and our ramdisk, so `/init` finds
the stock first-stage modules (UFS, clocks, regulators, USB) in `/lib/modules`.

## Why the kernel config is so constrained

The device's vendor modules are Xiaomi prebuilts compiled against the stock GKI
with `CONFIG_MODVERSIONS=y`. Our `Image` must export every symbol they use with
an identical CRC. Any option that changes an exported struct layout breaks that.
`tools/check-kmi.py` reads every stock `.ko`'s `__versions` table, compares it with
our `Module.symvers` and stops `./build.sh all` on any mismatch.

GKI lacks several things systemd and udev need. The ones that are layout-neutral
are enabled in `kernel/yupei-linux.fragment` (`DEVTMPFS`, `FHANDLE`, tmpfs ACL/xattr,
`AUTOFS`, `AF_ALG`). The rest stay off. User namespaces, SysV IPC and nftables are
therefore unavailable. This is checked by resolving the real Kconfig graph:
all fragment options survive `olddefconfig` on top of `gki_defconfig`.

The `-w-` branch follows HyperOS 3 / Android 16. The stock device kernel may be
a newer GKI sublevel than 6.6.57. If `check-kmi.py` reports CRC mismatches, the
published source lags the shipped firmware. Use a fastboot ROM that matches the
source release, or the GKI `Image` from that ROM's `boot.img` plus a custom ramdisk
(that avoids the rebuild but loses `DEVTMPFS`/`FHANDLE`, which systemd needs).

## Bring-up order

1. `./flash.sh --test`: RAM-boot, nothing written. Success = the USB NCM gadget shows
   up and `telnet 172.16.42.1` gives a shell. `dmesg`, `/run/initramfs.log`.
2. Flash, boot to systemd, SSH over USB.
3. `systemctl status systemd-modules-load`: which vendor modules fail and why
   (`dmesg | grep -iE 'disagrees|unknown symbol|firmware'`).
4. Display: `msm_drm` from `vendor_dlkm` is a downstream SDE KMS driver. Try
   `DESKTOP=sway` with `WLR_RENDERER=pixman sway` (kmscube is not packaged for ALARM).
5. GPU: KGSL, not msm DRM. Mesa's Turnip has a KGSL backend
   (`-Dfreedreno-kmds=kgsl`), but Arch's mesa is not built with it, and Adreno 8xx
   support must be verified. Expect software rendering first.
6. Wi-Fi (`qca_cld3` + `cnss2`, PCIe): firmware from the modem partition and
   `/usr/lib/firmware/wlan`. Android's `cnss-daemon` is not present, so features
   that rely on it may not work.
7. Audio: AudioReach/AGM/PAL userspace is proprietary. No ALSA UCM exists. Unsupported.
8. Suspend: masked until runtime PM of all blocks is understood.

## Debugging without a serial console

- Add `yupei.debug` to `EXTRA_CMDLINE` when running `tools/mkimages.sh`. The
  initramfs then starts the USB shell and waits for `touch /run/continue`.
- If the device reboots before USB is up, check `pstore`/ramoops after the next boot:
  `mount -t pstore pstore /sys/fs/pstore`.
