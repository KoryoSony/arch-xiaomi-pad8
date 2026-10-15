<div align="center">

<a href="README.md"><img alt="English" src="https://img.shields.io/badge/%E2%9C%93%20English-0057b8?style=for-the-badge&logo=googletranslate&logoColor=white" height="44"></a>
&nbsp;
<a href="README.zh-CN.md"><img alt="简体中文" src="https://img.shields.io/badge/%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87-6e7781?style=for-the-badge&logo=googletranslate&logoColor=white" height="44"></a>

</div>

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
| ✅ | **GNOME desktop** | GDM login, lock screen (one-finger swipe), power menu, auto-rotation, Arch boot splash |
| ✅ | **Touchscreen** | Custom decoder for the Xiaomi THP raw frames (`yupei-touchd`), 10-finger multi-touch, 120 Hz |
| ✅ | **Wi-Fi** | WCN7750, NetworkManager, Wi-Fi Direct capable |
| ✅ | **Bluetooth** | WCN7750 Bluetooth core over UART (patched `hci_qca`), real device address |
| ✅ | **Speakers** | All four SIA91xx speakers, stereo, Xiaomi tuning + speaker protection (AudioReach on the ADSP) |
| ✅ | **Microphones** | Through the same AudioReach stack (echo cancellation not yet) |
| ✅ | **Hardware video decoding** | H.264 / HEVC / VP9 on the Iris3 decoder (mpv, GStreamer/FFmpeg apps). Firefox: opt-in, see *Tips* |
| ✅ | **Battery & charging** | Real percentage, fast charging (QC 3.5), low-battery shutdown protection |
| ✅ | **Status LED** | Breathes white while booting, then green and off; while charging it breathes from red (low) to green (full) |
| ✅ | **Flashlight** | Both diodes, from a quick settings tile or the `flashlight` command |
| ✅ | **Brightness** | Smooth slider (perceptual curve), automatic brightness from the light sensor (quick settings tile) |
| ✅ | **Temperatures** | CPU, GPU, battery, charger, display and case, in a quick settings tile |
| ✅ | **Sensors** | Accelerometer (rotation), ambient light, proximity, compass (Qualcomm SSC) |
| ✅ | **Power button & cover** | Tap = lock + screen off, hold = power menu (also on the login and first-setup screens), magnetic cover locks/wakes |
| ✅ | **Volume keys** | |
| ✅ | **USB-C** | Host and device mode, USB networking for SSH |
| ✅ | **Power management** | Qualcomm's own CPU/bus tuning (walt, deep idle): ~40 mA with the screen off |
| ⚠️ | **External displays** | USB-C DisplayPort should work but is untested; Miracast not yet |
| ❌ | **Cameras** | Need Qualcomm's closed camera stack — not realistic on Linux |
| ❌ | **Stylus, keyboard cover** | Not decoded yet |
| ❌ | **Suspend** | The screen-off idle state is used instead (~40 mA) |
| ❌ | **IR blaster** | Not started |

## Install it (easy version)

> [!CAUTION]
> Installing **erases everything** on the tablet: Android, apps, photos.
> Save anything you want to keep first.

### Before you start (only once)

1. **Unlock the tablet's bootloader** with Xiaomi's *Mi Unlock* tool.
   Xiaomi makes you wait a few days the first time. If you already did this, skip it.
2. **Check the HyperOS version.** On the tablet open **Settings → About
   tablet**. It must say **OS4.0.5.0.XPZCNXM**. If it says something else,
   first install that version with Xiaomi's *fastboot ROM* for the Pad 8
   (`yupei`, China, OS4.0.5.0) and its `flash_all` script.
3. You need a **computer** (Windows, Mac or Linux) and a **USB-C cable**.

### Install

1. Open the **[Releases](../../releases)** page and download **all the files**
   of the newest release into **one folder** on your computer:
   `boot.img`, `init_boot.img`, `userdata.part1.img`, `userdata.part2.img`, …
   and the installer for your computer.
2. **Start the installer:**
   - **Windows:** double-click **`Install-Windows.bat`**
   - **Mac:** right-click **`Install-Mac.command`** → **Open**
   - **Linux:** run **`./install-linux.sh`** in a terminal
3. **Do what it says.** It will tell you to:
   - turn the tablet **off**,
   - hold **Power + Volume Down** until you see the **bunny** (fastboot mode),
   - plug the tablet into the computer.
4. When it asks, type **`YES`**. Wait a few minutes. **Don't unplug the cable.**
5. The tablet restarts by itself. The first start takes about a minute.
   Then it asks for your **language**, **Wi-Fi**, and a **name and password**
   for you. That's it!

The installer gets Google's official `fastboot` tool by itself, checks it is
really a Xiaomi Pad 8 and that it is unlocked, and changes nothing until you
type `YES`.

### Help!

- **The installer waits forever for the tablet (Windows):** install Xiaomi's
  USB driver (it comes with *Mi Unlock* or *Mi Flash*), then try again.
- **The tablet only shows the fastboot bunny after some restarts:** run
  `fastboot set_active a` then `fastboot reboot` (with the `fastboot` inside the
  `platform-tools` folder the installer made).
- **I want Android back:** install the stock fastboot ROM with Xiaomi's
  `flash_all` script, the same way as in step 2 of *Before you start*.

### For experts

The images are plain fastboot images (the current slot is used, the stock
`vbmeta` stays — this bootloader rejects a "verification disabled" one):
```sh
fastboot flash boot boot.img
fastboot flash init_boot init_boot.img
fastboot flash userdata userdata.part1.img   # then part2, part3, ... in order
fastboot reboot
```
The `userdata` parts are pieces of one sparse image (each one fills in its own
blocks and leaves the rest alone). `SHA256SUMS` has the checksums. Over a USB
cable the tablet is also a network adapter: `ssh alarm@172.16.42.1`
(password `alarm`, USB only — Wi-Fi SSH takes keys only).

## Tips

- **Video in Firefox:** Firefox plays video on the CPU by default. To use the
  hardware decoder (much less battery), open a terminal and type
  `firefox-hw-video on`, then restart Firefox (`firefox-hw-video off` undoes
  it). This turns off the sandbox of Firefox's video process, which is a bit
  less safe. For YouTube also install the *h264ify* extension.
- **Battery:** the backlight is the biggest consumer; 60 Hz instead of 120/144 Hz
  saves ~180 mA. Lock with the power button when you put it down.
- **Rotation lock, flashlight, auto brightness, temperatures:** in GNOME's
  quick settings (swipe down from the top-right corner).
- **Boot messages:** hold **Volume Up** while the Arch logo is showing, or
  create `/etc/yupei/verbose-boot` to always see them.
- **Bluetooth** starts switched off to save battery; turn it on in quick settings.

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
