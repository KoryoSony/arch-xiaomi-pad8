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

## Lessons from bring-up

These are the things that broke the first boots of yupei, in order:

1. **Kernel source**: the device runs Google's GKI, not a Xiaomi build. Find
   the tag from the `-g<sha>` in the stock version string (OS4.0.5.0:
   `android15-6.6-2026-01_r37`). Xiaomi's vendor modules (built at 6.6.57)
   load on it unchanged: all 852 pass `check-kmi.py`.
2. **vbmeta**: Xiaomi's OS4 bootloader *rejects* boot images ("Failed to
   load/authenticate boot image") when vbmeta is flashed with verification
   disabled. Keep the stock vbmeta; unlocked, it accepts unsigned images.
3. **`fastboot boot` ignores its ramdisk**: the bootloader always takes the
   ramdisk from the `init_boot` partition. RAM-booting a test image only
   swaps the kernel (it booted HyperOS on our kernel).
4. **Ramdisk compression**: the vendor ramdisk is lz4-legacy, so ours must be
   too. A gzip segment after it is silently not unpacked.
5. **Module loading**: `modules.load` is not dependency-ordered. Load each
   module's `modules.dep` closure (deepest first) and `softdep pre:`, pass
   `<module>.<param>=` arguments from the cmdline, skip `modules.blocklist`.
   Without this, UFS never probes and the boot stalls on the Mi logo.
6. **USB gadget**: dwc3-msm only creates its UDC in device mode, which normally
   comes from the USB-C role via pmic_glink on the ADSP. Force
   `/sys/devices/platform/soc/*.ssusb/mode` to `peripheral`, and start the
   ADSP once the modem partition is mounted.
7. **Recovery**: once our lz4 `init_boot` is valid, it also replaces TWRP's
   `/init` in recovery boots. Flash a deliberately unparseable `init_boot` to
   get TWRP back. `tools/probe-init.sh`, `tools/mkimages-probe.sh` and
   `tools/run-probe.sh` locate hangs with reboot-loop checkpoints when nothing
   is visible.
8. **Power key**: logind powers off on a short press. That's the clean way to
   stop the tablet; hold Power + Volume Down to get back to fastboot.
9. **Display**: the panel (dual DSI + DSC) needs one plane per half. A single
   full-width plane modesets fine but underruns (grey screen, colours only at
   the top) at any refresh rate. `tools/kmsatomic.c` shows the two-plane
   commit; `tools/yupei-present.c` runs sway on wlroots' headless backend and
   copies damaged frames (wlr-screencopy) to a two-plane scanout.
   `/usr/lib/yupei/start-sway` launches it (seatd needs `SEATD_VTBOUND=0`:
   there are no VTs).
10. **pacman**: the GKI kernel lacks Landlock, so set `DisableSandbox`.
11. **GNOME**: `yupei-present-gnome` + `start-gnome` run `gnome-shell --headless
    --virtual-monitor 3200x2136 --no-x11`, record Meta-0 over the Mutter
    ScreenCast/RemoteDesktop D-Bus APIs (PipeWire, linked by port name) and
    forward touches as RemoteDesktop touch events. Pitfalls found on the way:
    - **SVE**: these Cortex-A720/A520 cores have SVE disabled, but LLVM infers
      it from the core type, so llvmpipe JIT code dies with SIGILL
      (`index z1.s`). `yupei-cpuinfo.service` presents a Cortex-A76 in
      /proc/cpuinfo and the MIDR_EL1 sysfs files; Mesa's shader cache must be
      cleared of code built before that.
    - `--no-x11`: otherwise libgvc's PulseAudio client connects to the
      on-demand Xwayland from GNOME's main loop and deadlocks it.
    - Solid background: glycin's sandbox (bubblewrap) cannot run without
      user namespaces.
12. **Touch**: the NT36532 controller runs Xiaomi THP. It streams raw
    capacitance frames (60x40) and `libtouchreport_hal.so` (Android, odm)
    computes the points and hands them back to the driver. Without that
    userspace, no input events are produced. Its firmware
    (`novatek_nt36532_yupei_fw_boe.bin`, odm) is reloaded on every resume.
13. **Touch decoder**: `tools/yupei-touchd.c` replaces Xiaomi's THP HAL. It
    reads frames from `/dev/xiaomi-touch` (ioctl magic 'T': SELECT_TOUCH_ID 3,
    HARDWARE_PARAM 1, SELECT_MMAP 2 with area 1 = frames, GET_FRAME_DATA_INDEX
    4; `poll()` POLLPRI per frame). Finger frames (type 3) carry a 60x40 int16
    matrix at tp_raw+64; a finger raises cells by ~1000 over a ~10000
    baseline (noise ~20). Column 0..59 = screen left..right, row 39..0 =
    top..bottom. Local maxima + 3x3 centroid + nearest-neighbour tracking,
    published as the uinput multi-touch device `yupei-thp-touch`, which the
    presenter forwards to GNOME. `tools/thp-capture.c` inspects raw frames.
14. **GPU**: Adreno 825 = KGSL chip id 0x44030000, same 0x4403 core as the
    Adreno 829 Mesa already knows (4 CCU / 2 slices, a8xx gen2); GMEM 2432 KiB
    comes from KGSL. `mesa/build-mesa.sh` builds Mesa main with Turnip
    (`-Dfreedreno-kmds=msm,kgsl`: kgsl alone drops DRM and with it GBM) and
    Zink into /opt/mesa-yupei, adding the 825 to freedreno_devices.py.
    `tools/gpu-bench.c`: 3200x2136 full-screen shader passes, 575 fps on the
    GPU vs 61 fps on llvmpipe, pixel-verified. Turnip/KGSL refuses instances
    that enable VK_KHR_display (vulkaninfo does); Zink does not.
    gnome-session exports the Mesa paths and MESA_LOADER_DRIVER_OVERRIDE=zink;
    mutter then creates its GBM renderer on renderD128 through Zink.
    `touch /etc/yupei/no-gpu` falls back to llvmpipe.
15. **Wi-Fi**: as in Android's init.vendor.wlan.rc, `icnss2` must boot WPSS
    (`/sys/kernel/icnss/wlan_en_delay` 1000, `wpss_boot` 1), not remoteproc
    directly; then `qca_cld3_wcn7750` + `printf ON > /dev/wlan`. The driver
    probe fails ("DMS QMI connection not established") unless it finds the
    MAC in wlan/qca_cld/wcn7750/wlan_mac.bin -> /mnt/vendor/persist/wlan/
    (this Wi-Fi-only tablet has no modem DMS), and needs
    /vendor/etc/wifi/wcn7750/WCNSS_qcom_cfg.ini. persist is mounted read-only.
    Do not rmmod the driver after a failed probe: that crashed the kernel.
16. **Session robustness**: GNOME's XDG_RUNTIME_DIR is /run/yupei-desktop;
    /run/user/0 is emptied by user-runtime-dir@0 whenever a root login ends,
    which removed the Wayland socket ("Failed to open display" for new apps).
    upower needs `PrivateUsers=no` (no user namespaces on GKI).
17. **Login session**: yupei-desktop.service uses `PAMName=yupei-desktop`
    (pam_permit + pam_systemd) with XDG_SESSION_TYPE=wayland/XDG_SEAT=seat0,
    so GNOME runs in an active logind session. Without it NetworkManager
    ignores GNOME Shell's secret agent (no Wi-Fi password prompt) and polkit
    refuses its authentication agent.
18. **Wi-Fi password prompt**: GNOME Shell's network agent first looks the
    secret up in the keyring; without a Secret Service (gnome-keyring) the
    lookup errors out and no dialog is shown ("Internal error while
    retrieving secrets from the keyring").
19. **One desktop only**: logind keeps PAM sessions in their own scope and
    KillUserProcesses=no keeps processes after terminate-session, so
    `desktop-cleanup` (ExecStartPre/ExecStopPost) kill-session's old seat0
    sessions; otherwise every restart left an invisible gnome-shell running.
20. **Cast icon**: the panel is a RemoteDesktop session, so GNOME showed
    "screen is being shared" permanently. gnome-session patches the installed
    status/remoteAccess.js (gresource extract + sed, applied via
    G_RESOURCE_OVERLAYS) so the indicator ignores MetaRemoteDesktopSessionHandle;
    portal ScreenCast sharing (browsers, calls) still shows it. Skipped if the
    upstream code no longer matches.
21. **Power menu / power key**: GNOME Shell asks org.gnome.SessionManager
    (gnome-session) whether it can shut down. `yupei-session-manager.js` (gjs)
    owns that name, answers AVAILABLE, opens GNOME's EndSessionDialog and runs
    systemctl poweroff/reboot on ConfirmedShutdown/ConfirmedReboot. logind
    ignores the power key; `yupei-powerkey` maps a short press to screen
    off/on (SIGUSR1 to the presenter: CRTC off + mutter PowerSaveMode) and a
    0.7 s hold to the power-off dialog. The panel's session shows up as a
    MetaScreenCastSessionHandle + MetaRemoteDesktopSessionHandle pair; the
    indicator patch skips handles paired within 2 s.
22. **Boot slot**: the bootloader gives each A/B slot 7 attempts and Android
    marks a boot successful once it is up. Without that, slot a ends
    unbootable after 7 Arch boots and the tablet stays in fastboot
    (`fastboot getvar slot-unbootable:a` = yes; `fastboot set_active a`
    resets it). `yupei-boot-success.service` runs `qbootctl -m`
    (qbootctl/build-qbootctl.sh) on every boot.
23. **Native GNOME (GDM)**: replaces the presenter. Needs (a) our Mesa ahead of
    Arch's for every program (`/etc/ld.so.conf.d/00-yupei-mesa.conf`): Arch's
    GBM on msm_drm makes buffers the display rejects (drmModeAddFB ENXIO), so
    the panel stays dark; (b) mutter patch 0002: msm_drm planes advertise 10
    bpc formats but AddFB fails on them, so use 8 bpc; (c) `TAG+="uaccess"` on
    kgsl/dma_heap, since GDM's greeter runs as a dynamic user
    (gdm-greeter, uid 60578) outside the render group.
24. **Power key / cover**: logind ignores both; `yupei-powerkey` (session
    autostart) reads pmic_pwrkey and the xiaomi-hall SW_LID. It samples the
    panel's DPMS state on key *down*: mutter wakes the panel on any key, so
    asking after the press always says "on" and blanked it again. Tap: lock +
    PowerSaveMode off, or wake; hold: power-off dialog; cover: lock + off / on.
25. **GDM login hang**: without VTs, logind makes the user session active as
    soon as GDM opens it, taking the display from the greeter; stock GNOME
    Shell starts the session only after the greeter's fade-out completes, which
    can then never render. `shell-overlays` (boot service) patches
    loginDialog.js to start the session first, loaded via G_RESOURCE_OVERLAYS
    (/etc/environment and environment.d). Autologin never had the problem.
    The greeter reads its monitor config (scale, refresh rate) from
    /var/lib/gdm/seat0/config/monitors.xml; the on-screen keyboard is enabled
    via the gdm dconf profile (system-db:gdm).
26. **Battery safety**: the MCA charger driver signals CAPACITY_LEVEL_CRITICAL
    ("Trigger Android shutdown") but nothing on Linux acted (UPower's Auto
    action needs suspend/hibernate): the battery ran down to 2.5 V.
    `yupei-battery-guard` powers off below 3.3 V (fuel gauge
    xm_power/fg_master/vbatt, rsoc); UPower CriticalPowerAction=PowerOff.
    The displayed percentage (MCA UISOC) is restored from the last shutdown
    and does not fall while charging, so after a deep discharge it reads high
    until the real SOC (fg_master/rsoc) catches up; writes to capacity are
    ignored. Xiaomi fast charging is gated by pd_verifed (set on Android by a
    userspace adapter-authentication service).
    Fixed since: `strategy_fg/fake_soc` (writable) overrides the reported
    capacity, so battery-guard mirrors `raw_soc` (0.01 %) into it every 5 s.
    MCA's current_now is negative while charging, which UPower takes as
    "discharging"; battery-guard writes the negated value to
    /run/yupei/battery/current_now, bind-mounted over the sysfs file in
    upower.service only (path via /sys/class: a ':' breaks BindReadOnlyPaths).
27. **Audio, stage 1 (sound card)**: the ADSP's audio PD stays down ("q6 state
    is down") without a service locator, and its audio framework (SPF/APM)
    never answers unless the audio PD listener runs while the ADSP boots.
    Android order, reproduced: yupei-firmware (mounts) -> yupei-pd-mapper
    (linux-msm pd-mapper; patched: it chopped the last character of a
    firmware path written without a newline) + yupei-audiopd (quic/fastrpc
    `adsprpcd audiopd adsp`, ADSP_LIBRARY_PATH=/vendor/dsp/adsp) ->
    yupei-dsp (starts ADSP/CDSP). Result: card `tuna-mtp-snd-card` with 54
    back-end PCMs. Playback needs the AudioReach userspace (ARGS/GSL + AGM +
    PAL) to build DSP graphs from the ACDB (vendor/etc/acdbdata,
    odm/etc/acdbdata/Mi/Mi_acdb_cal.acdb): next stage.
28. **Audio, stage 2 (AudioReach userspace)**: audioreach/build-audioreach.sh
    builds tinyalsa (CMake, with plugins: PAL's virtual card 100 is served by
    AGM's tinyalsa plugins), graph services (`--with-qcom
    --with-audio_dma_support`: DMA-BUF from /dev/dma_heap/system mapped by
    Xiaomi's audpkt_ion, /dev/msm_audio_ion), AGM, PAL and the PipeWire
    plugin. audioreach/device-configs.sh adapts Xiaomi's odm configs
    (resourcemanager needs <config_max_sessions> on Linux; Android-only
    context manager / connectivity proxy off) and installs the Mi ACDB (AGM
    takes exactly one .acdb + .qwsp). udev: aud_pasthru_adsp, apr_apps2,
    msm_audio_ion* -> audio group. WirePlumber: v4l2/libcamera monitors off
    (Qualcomm camera nodes stalled its event queue: nothing was ever linked).
    Result: deep-buffer playback to
    the speaker graph (TDM to the SIA91xx amps, speaker protection on).
    Speakers: four SIA91xx amps (SpkrLeft/Right T/B "Sipa" controls); their
    scene/mute settings live in Xiaomi's mixer_paths_overlay_static.xml, which
    Xiaomi's Android HAL merges into the mixer paths. Without it only one amp
    played; audioreach/merge-mixer-overlay.py merges it the same way.
    Stereo layout: the speaker device is 4 TDM slots (PAL map FL FR LB RB);
    slot 0 = bottom-left, 1 = top-left, 2 = bottom-right, 3 = top-right (landscape). The
    plugin only filled FL/FR for playback, so the right amps were silent:
    patched to send the full 4-channel map, and the Speakers sink is 4
    channels positioned RL FL RR FR with PipeWire's simple upmix (left on
    both left speakers, right on both right). Not orientation-aware yet.
    Charging icon: UPower re-reads the battery only on a uevent, which the
    kernel sends at plug-in before battery-guard had corrected current_now,
    so it showed "discharging". battery-guard now follows the status every
    second (sign from status, not current) and sends a change uevent when the
    status flips.
29. **Security defaults**: no GDM auto-login (it was a debugging leftover),
    wheel sudo with the user's password (no NOPASSWD), root locked, SSH keys
    only except over the USB network (172.16.42.1) for first-time setup of a
    fresh image (alarm/alarm). setup-desktop-user asks for the password.
30. **Power**: the GKI kernel boots with the "performance" cpufreq governor
    (every core at full clock) and qcom_lpm `sleep_disabled=1` (no deep CPU
    idle): ~1.3 A drawn while idle. Android's
    init.kernel.post_boot-tuna_default_2_3_2_1.sh (walt governor + tunables,
    core_ctl, min freqs, LLCC/DDR bwmon, deep idle, s2idle) is copied from the
    stock vendor image and run by yupei-power-tuning.service with Android's
    setprop/getprop stubbed. Video is still decoded in software (Firefox).
31. **Idle power by component** (screen on, 23 % backlight): 120 Hz 1.11 A,
    60 Hz 0.93 A, 30 Hz 0.83 A; backlight dominates (2 % 0.64 A, 50 % 1.8 A);
    screen off 40 mA. The panel switches 30-144 Hz modes without flicker
    (an idle-based automatic switcher was tried and removed at the user's
    request; the rate is whatever is set in Settings).
32. **Bluetooth (WCN7750 "Orne" core on /dev/ttyHS0)**: no serdev node, so
    `yupei-bluetooth` attaches it with btattach and patched modules
    (kernel/patches): hci_uart.qca_soc=wcn7850 (same HCI/TLV protocol),
    btqca.bdaddr= (address from persist bluetooth/.bt_nv.bin written into the
    NVM while downloading; the NVM's placeholder stays the "unconfigured"
    reference), btqca.nosleep / hci_uart.qca_ibs=0 (no in-band sleep),
    hci_uart.qca_memdump=0 (a forced controller crash dump on a timeout reset
    the whole tablet). Firmware is ORNE 1.0 (ornbtfw10.tlv / ornnv10.bin on
    the bluetooth partition); Linux derives ROM 1.0 and asks for
    hmtbtfw10.tlv / hmtnv10.bin, so those names link to it (the hmt*20 files
    are Hamilton 2.0 and make the chip reset). The msm_geni_serial UART
    runtime-suspends 150 ms after use (Android holds a clock vote): kept
    power/control=on while attached, "auto" for each attach (reopening needs
    a resume). The chip keeps its UART speed over power toggles (rails shared
    with Wi-Fi): 3 Mbps is tried first, then 115200. bt_power (the chip's
    power) is a bluetooth rfkill that GNOME's toggle blocks too, so the
    service detaches on block and re-attaches on unblock.
33. **Touch decoder v2** (tools/yupei-touchd.c): every frame is processed (no
    "newest only"), contacts get Gaussian sub-cell positions (no inward bias
    at the edges), fingers are matched around their predicted position
    (radius 160 px + speed; separate keys are >= 250 px apart) so quick taps
    on different keys never merge into one drag, down at the first frame
    above 260, up below 140, 1-euro smoothing. `--replay` runs recordings
    (YUPEI_TOUCH_RECORD=file) or tools/touchsim.py scenarios offline.
34. **Sensors / rotation**: accelerometer (LSM6DSO), light, proximity and
    compass are in the ADSP's sensor core (SSC, QMI service 400 over QRTR).
    iio-sensor-proxy (Arch, built with libssc) reads them once a udev rule
    sets IIO_SENSOR_PROXY_TYPE=ssc-* (on /dev/fastrpc-adsp-secure). SSC's
    registry was "unavailable" until `adsprpcd sensorspd adsp` (Android's
    sscrpcd) runs before the ADSP boots, with /vendor/etc/sensors,
    /odm/etc/sensors and /vendor/lib/rfsa/adsp from the stock images; it
    rewrites the registry in persist, so persist/sensors gets an overlay in
    /var/lib/yupei/persist-sensors (factory partition untouched). Mutter
    rotates on the GPU (planes stay rotation=0), so split scanout still works.
35. **Firefox GPU + video**: our standalone libEGL (non-glvnd Mesa) left
    Firefox's desktop GL (through glvnd's libGL) without a driver: its
    contexts failed their init checks and WebRender fell back to software.
    Mesa is now built with glvnd (libEGL_mesa.so.0 vendor, first in the
    ld.so path). Firefox's V4L2-DRM decoder then works, but its RDD sandbox
    blocks /dev/dma_heap: the FFmpeg patch falls back to linear GBM buffers
    from /dev/dri/renderD128 (allowed), equally bit-exact.
