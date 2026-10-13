# Arch Linux ARM for the Xiaomi Pad 8 (`yupei`)

A port of Arch Linux ARM to the Xiaomi Pad 8. It uses the kernel source Xiaomi
published ([`MiCode/Xiaomi_Kernel_OpenSource@yupei-w-oss`][kernel], Linux 6.6 GKI,
Qualcomm `sun` platform) together with the vendor drivers and firmware from the stock ROM.

> **Status: boots on hardware** (2026-10-13, HyperOS OS4.0.5.0.XPZCNXM base,
> kernel `6.6.118-4k-arch+` built from Google's `android15-6.6-2026-01_r37`).
> Reaches `multi-user.target` in ~90 s; SSH over USB works.

| Component | State on first boot |
|---|---|
| Boot, systemd, SSH over USB (172.16.42.1) | ✅ Works |
| Storage, root fs auto-grown to fill userdata | ✅ Works |
| ADSP / CDSP remoteprocs | ✅ Started by `yupei-firmware.service` |
| Battery, charger, USB-C (pmic_glink / UCSI) | ✅ `power_supply` nodes present |
| Touchscreen (Novatek) + pens, hall sensor, keys | ✅ Input devices present |
| Display (downstream `msm_drm`, DSI 3200x2136@120) | ✅ sway desktop via `yupei-present` (two-plane split, CPU rendering) |
| GPU acceleration (KGSL) | ❌ Needs Mesa built with the KGSL backend |
| Wi-Fi (WCN7750, `wpss` remoteproc) / Bluetooth | ❌ Not started yet |
| Audio, camera, suspend | ❌ Not supported |

Hard-won notes for this device are in [docs/PORTING.md](docs/PORTING.md#lessons-from-bring-up).

## Requirements

- Unlocked bootloader (Mi Unlock). This erases the device.
- A **HyperOS 3 (Android 16) fastboot ROM** (`yupei_*_images_OS3.*.tgz`), and the
  tablet running that same build. Xiaomi only published kernel source for that
  release; `./build.sh stock` refuses ROMs whose kernel series differs.
- Docker (or an Arch Linux x86_64/aarch64 machine) for building, plus `fastboot`.

## Build

**Apple Silicon / arm64 Linux (native, fastest):**
```sh
./docker/build-image.sh      # official Arch Linux ARM tarball, clang 18
docker run --rm -it --privileged --platform linux/arm64 \
    -v "$PWD:/port" -v yupei-work:/port/work yupei-build-arm64 \
    ./build.sh all /port/yupei_global_images_OS3.x.x.x.WPZMIXM.tgz
```

**x86_64 Linux** (uses Google's pinned clang r510928):
```sh
docker build -t yupei-build .
docker run --rm -it --privileged -v "$PWD:/port" -v yupei-work:/port/work yupei-build \
    ./build.sh all /port/yupei_global_images_OS3.x.x.x.WPZMIXM.tgz
```
`DESKTOP=sway` adds sway (software rendering). If clang warns where Google's
toolchain did not, rerun with `-e KERNEL_NO_WERROR=1`.

Steps: extract stock vendor modules and firmware, build the kernel, then **verify
every stock module's symbol CRCs against the new kernel** (the build stops on a
mismatch). After that it builds the rootfs and initramfs and packs
`out/{boot,init_boot,userdata,boot-test}.img`.

## Install

```sh
adb reboot bootloader
./flash.sh --test   # RAM-boot only; then: telnet 172.16.42.1, 'touch /run/continue'
./flash.sh          # writes boot, init_boot, vbmeta (AVB off), userdata (erases Android data)
ssh alarm@172.16.42.1   # password alarm; root/root. Change both.
```

Back to Android: `./restore-android.sh`, or Xiaomi's `flash_all.sh` from the fastboot ROM.

## Layout

```
build.sh                    orchestrates all steps
kernel/yupei-linux.fragment KMI-safe options systemd needs (DEVTMPFS, FHANDLE, ...)
kernel/build-kernel.sh      GKI Image + modules, stubs Xiaomi's unpublished dirs
tools/extract-stock.sh      vendor_dlkm/vendor/vendor_boot from the fastboot ROM
tools/check-kmi.py          offline modversions/vermagic check against stock modules
initramfs/init              loads first-stage modules, finds userdata, switch_root
rootfs/                     ALARM image + firmware mounts, vendor module loading, USB gadget
tools/mkimages.sh           header-v4 boot/init_boot images
flash.sh, restore-android.sh
```

[kernel]: https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/yupei-w-oss
