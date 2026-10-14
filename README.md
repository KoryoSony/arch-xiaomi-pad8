<div align="center">

# Arch Linux ARM for the Xiaomi Pad 8

**`yupei` · Snapdragon 8s Gen 4 (SM8735) · Adreno 825 · 3.2K 144 Hz**

Full desktop Linux — GNOME on the GPU, sound, Wi-Fi, Bluetooth, touch, sensors —
on Xiaomi's 2025 tablet, built on the vendor's own GKI kernel.

![status](https://img.shields.io/badge/status-ALPHA-orange)
![kernel](https://img.shields.io/badge/kernel-6.6.118%20GKI-blue)
![desktop](https://img.shields.io/badge/desktop-GNOME%2051-4a86cf)
![gpu](https://img.shields.io/badge/GPU-Turnip%20%2B%20Zink-green)

</div>

> [!WARNING]
> **Alpha build.** This erases Android, it is not as polished as HyperOS, and
> things will break. You need an unlocked bootloader and the matching stock
> ROM to get back. Read the whole install section first.

---

## What works

| | Feature | Notes |
|---|---|---|
| ✅ | **Display** | 3200×2136 native panel, 30–144 Hz, patched mutter (split scanout for the dual-DSI panel) |
| ✅ | **GPU** | Adreno 825: Mesa Turnip (Vulkan 1.4) + Zink (OpenGL 4.6 / GLES 3.2), GNOME and Firefox render on the GPU |
| ✅ | **GNOME desktop** | GDM login, lock screen (one-finger swipe), power menu, auto-rotation |
| ✅ | **Touchscreen** | Custom decoder for the Xiaomi THP raw frames (`yupei-touchd`), 10-finger multi-touch, 120 Hz |
| ✅ | **Wi-Fi** | WCN7750, NetworkManager, Wi-Fi Direct capable |
| ✅ | **Bluetooth** | WCN7750 Bluetooth core over UART (patched `hci_qca`), real device address |
| ✅ | **Speakers** | All four SIA91xx speakers, stereo, Xiaomi tuning + speaker protection (AudioReach on the ADSP) |
| ✅ | **Microphones** | Through the same AudioReach stack (echo cancellation not yet) |
| ✅ | **Hardware video decoding** | H.264 / HEVC / VP9 on the Iris3 decoder; Firefox uses it for H.264 (use the *h264ify* extension for YouTube) |
| ✅ | **Battery & charging** | Real percentage, fast charging (QC 3.5), low-battery shutdown protection |
| ✅ | **Sensors** | Accelerometer (rotation), ambient light, proximity, compass (Qualcomm SSC) |
| ✅ | **Power button & cover** | Tap = lock + screen off, hold = power menu, magnetic cover locks/wakes |
| ✅ | **Volume keys** | |
| ✅ | **USB-C** | Host and device mode, USB networking for SSH |
| ✅ | **Power management** | Qualcomm's own CPU/bus tuning (walt, deep idle): ~40 mA with the screen off |
| ⚠️ | **External displays** | USB-C DisplayPort should work but is untested; Miracast not yet |
| ❌ | **Cameras** | Need Qualcomm's closed camera stack — not realistic on Linux |
| ❌ | **Stylus, keyboard cover** | Not decoded yet |
| ❌ | **Suspend** | The screen-off idle state is used instead (~40 mA) |
| ❌ | **IR blaster** | Not started |

## Downloads

Grab the files from the **[Releases](../../releases)** page:

| File | Flash to |
|---|---|
| `boot.img` | `boot` — Linux kernel |
| `init_boot.img` | `init_boot` — initramfs |
| `userdata.img.zst` (split into `.part-*` if large) | `userdata` — the Arch Linux system (sparse image) |
| `SHA256SUMS` | checksums |

## Requirements

- **Xiaomi Pad 8 (`yupei`)** with an **unlocked bootloader** (Mi Unlock).
- **HyperOS OS4.0.5.0.XPZCNXM** installed. The kernel and vendor modules must
  match this exact build. Flash it first with Xiaomi's fastboot ROM
  (`flash_all.sh`) if you are on anything else — **this also keeps the stock
  `vbmeta`, which this bootloader requires** (a "verification disabled"
  vbmeta is rejected and the tablet won't boot).
- A computer with `fastboot` (Android platform-tools) and `zstd`.

## Install

> All Android data is erased.

1. **Get the images** and check them:
   ```sh
   cat userdata.img.zst.part-* > userdata.img.zst   # only if it was split
   sha256sum -c SHA256SUMS
   zstd -d userdata.img.zst                         # -> userdata.img
   ```
2. **Reboot to fastboot**: power off, then hold **Power + Volume Down**.
3. **Flash** (current slot, stock vbmeta stays):
   ```sh
   fastboot getvar current-slot     # just to see it; flash goes to the current slot
   fastboot flash boot boot.img
   fastboot flash init_boot init_boot.img
   fastboot flash userdata userdata.img
   fastboot reboot
   ```
4. **First boot** takes about a minute (the root filesystem grows to fill the
   storage). **GNOME Initial Setup** then asks for your language, Wi-Fi and a
   user account with a password.

### Remote access

Plug the tablet into a computer over USB-C: it shows up as a USB network
adapter. For first-time setup you can log in with `ssh alarm@172.16.42.1`
(password `alarm`) — **passwords only work over the USB cable**, Wi-Fi SSH is
key-only. Change or lock that account after setup (`sudo passwd -l alarm`).

### If something goes wrong

- **Stuck in fastboot after boots:** `fastboot set_active a && fastboot reboot`
  resets the boot slot (fixed in this build: the slot is marked successful on
  every boot).
- **Back to Android:** flash the stock ROM with Xiaomi's `flash_all.sh`.

## Tips

- **YouTube in Firefox:** install the *h264ify* extension so videos play on the
  hardware decoder instead of the CPU.
- **Battery:** the backlight is the biggest consumer; 60 Hz instead of 120/144 Hz
  saves ~180 mA. Lock with the power button when you put it down.
- **Rotation lock:** in GNOME's quick settings.

## Building it yourself

Everything is built from source in a Docker container (native on Apple Silicon /
arm64), with the vendor firmware and modules extracted from the stock ROM:

```sh
./docker/build-image.sh
docker run --rm -it --privileged --platform linux/arm64 \
    -v "$PWD:/port" -v yupei-work:/port/work yupei-build-arm64 \
    ./build.sh all /port/yupei_images_OS4.0.5.0.XPZCNXM.tgz
```

Components built from source and patched for this device: the kernel
(Bluetooth patches in `kernel/patches/`), Mesa (Adreno 825), mutter (split
scanout, 8 bpc), FFmpeg (V4L2 dma-buf + DRM PRIME for the Qualcomm decoder),
AudioReach (PAL/AGM/GSL + PipeWire), FastRPC, pd-mapper, qbootctl and the
touch decoder. The bring-up notes — 30+ device-specific gotchas and how they
were solved — are in **[docs/PORTING.md](docs/PORTING.md)**.

## Credits

Port by **koryo**. Built on the work of Arch Linux ARM, the Mesa/Freedreno
developers, the AudioReach and linux-msm projects, postmarketOS (libssc,
qbootctl, pd-mapper) and Xiaomi's kernel source release.

Vendor firmware, DSP modules and calibration files are Xiaomi's/Qualcomm's and
are extracted from the official stock ROM.
