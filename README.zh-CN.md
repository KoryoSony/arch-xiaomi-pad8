<div align="center">

# 小米平板 8 的 Arch Linux ARM

**`yupei` · 骁龙 8s Gen 4 (SM8735) · Adreno 825 · 3.2K 144 Hz**

完整的桌面 Linux：GPU 加速的 GNOME、声音、Wi-Fi、蓝牙、触摸、传感器，
运行在小米 2025 年的平板上，基于小米官方的 GKI 内核。

[English](README.md) · **简体中文**

![status](https://img.shields.io/badge/状态-ALPHA%20测试版-orange)
![kernel](https://img.shields.io/badge/内核-6.6.118%20GKI-blue)
![desktop](https://img.shields.io/badge/桌面-GNOME%2051-4a86cf)
![gpu](https://img.shields.io/badge/GPU-Turnip%20%2B%20Zink-green)

</div>

> [!WARNING]
> **这是 Alpha 测试版。** 安装后会清除安卓系统，体验不如澎湃 OS 流畅，
> 也一定会有 bug。你需要已解锁的 Bootloader，以及对应版本的官方线刷包才能刷回去。
> 请先把下面的安装说明全部读完。

---

## 能用什么

| | 功能 | 说明 |
|---|---|---|
| ✅ | **屏幕** | 3200×2136 原生分辨率，30–144 Hz |
| ✅ | **GPU** | Adreno 825：Mesa Turnip (Vulkan 1.4) + Zink (OpenGL 4.6 / GLES 3.2)，GNOME 和 Firefox 都用 GPU 渲染 |
| ✅ | **GNOME 桌面** | GDM 登录、锁屏（单指上滑）、电源菜单、自动旋转 |
| ✅ | **触摸屏** | 自制的小米 THP 触摸解码器 (`yupei-touchd`)，10 点触控，120 Hz |
| ✅ | **Wi-Fi** | WCN7750，NetworkManager，支持 Wi-Fi Direct |
| ✅ | **蓝牙** | WCN7750 蓝牙（打过补丁的 `hci_qca`），真实蓝牙地址 |
| ✅ | **扬声器** | 四个扬声器全部可用，立体声，小米调音 + 扬声器保护 |
| ✅ | **麦克风** | 可用（暂无回声消除） |
| ✅ | **硬件视频解码** | H.264 / HEVC / VP9（mpv、GStreamer/FFmpeg 程序）。Firefox 需要手动开启，见 *小技巧* |
| ✅ | **电池和充电** | 真实电量百分比，快充 (QC 3.5)，低电量自动关机保护 |
| ✅ | **传感器** | 加速度计（自动旋转）、环境光、距离、指南针 |
| ✅ | **电源键和保护套** | 短按 = 锁屏并熄屏，长按 = 电源菜单，磁吸保护套自动锁屏/唤醒 |
| ✅ | **音量键** | |
| ✅ | **USB-C** | 主机和设备模式，USB 网络（SSH） |
| ✅ | **电源管理** | 高通自己的 CPU/总线调优：熄屏约 40 mA |
| ⚠️ | **外接显示器** | USB-C DisplayPort 应该能用，但还没测试；无线投屏暂不支持 |
| ❌ | **摄像头** | 需要高通的闭源相机栈，在 Linux 上不现实 |
| ❌ | **手写笔、键盘保护套** | 还没做 |
| ❌ | **休眠** | 用熄屏省电状态代替（约 40 mA） |
| ❌ | **红外遥控** | 还没开始 |

## 安装（简单版）

> [!CAUTION]
> 安装会**清除平板上的一切**：安卓、应用、照片。
> 请先备份你想保留的东西。

### 开始之前（只需要做一次）

1. **解锁平板的 Bootloader**，用小米官方的 *小米解锁工具 (Mi Unlock)*。
   第一次解锁需要等几天。已经解锁过就跳过这一步。
2. **检查澎湃 OS 版本。** 在平板上打开 **设置 → 我的设备 / 关于平板**，
   必须是 **OS4.0.5.0.XPZCNXM**。如果不是，先用小米平板 8
   （`yupei`，国行，OS4.0.5.0）的*线刷包*和里面的 `flash_all` 脚本刷到这个版本。
3. 你需要一台**电脑**（Windows、Mac 或 Linux）和一根 **USB-C 数据线**。

### 安装

1. 打开 **[Releases（发布）](../../releases)** 页面，把最新版本的**所有文件**
   下载到电脑上的**同一个文件夹**里：
   `boot.img`、`init_boot.img`、`userdata.part1.img`、`userdata.part2.img`……
   以及适合你电脑的安装程序。
2. **运行安装程序：**
   - **Windows：** 双击 **`Install-Windows.bat`**
   - **Mac：** 右键点击 **`Install-Mac.command`** → **打开**
   - **Linux：** 在终端里运行 **`./install-linux.sh`**
3. **按照提示操作。** 它会让你：
   - 把平板**关机**，
   - 按住 **电源键 + 音量减**，直到看到**兔子**图案（fastboot 模式），
   - 用数据线把平板连到电脑。
4. 提示时输入 **`YES`**。等几分钟。**不要拔线。**
5. 平板会自己重启。第一次开机大约需要一分钟。
   然后它会让你选择**语言**、连接 **Wi-Fi**，并设置你的**用户名和密码**。完成！

安装程序会自动下载 Google 官方的 `fastboot` 工具，检查设备确实是小米平板 8
并且已经解锁。在你输入 `YES` 之前，它不会改动任何东西。

### 遇到问题？

- **安装程序下载 fastboot 失败：** 安装程序使用 `dl.google.com`。如果下载不了，
  手动下载 Google 的 *Android SDK Platform-Tools*，解压后把 `platform-tools`
  文件夹放到镜像所在的文件夹里，再运行一次安装程序。
- **GitHub 下载很慢：** 可以换个网络或用下载工具多线程下载。下载完后可以用
  `SHA256SUMS` 检查文件是否完整。
- **安装程序一直在等平板 (Windows)：** 安装小米的 USB 驱动（*小米解锁工具*
  或 *MiFlash* 里自带），然后再试。
- **平板重启几次后只显示 fastboot 兔子：** 运行 `fastboot set_active a`，再运行
  `fastboot reboot`（用安装程序生成的 `platform-tools` 文件夹里的 `fastboot`）。
- **我想刷回安卓：** 和 *开始之前* 第 2 步一样，用小米官方线刷包和 `flash_all` 脚本刷回去。

### 高级用户

这些是普通的 fastboot 镜像（刷到当前槽位，保留官方 `vbmeta`，这个 Bootloader
不接受“关闭验证”的 vbmeta）：
```sh
fastboot flash boot boot.img
fastboot flash init_boot init_boot.img
fastboot flash userdata userdata.part1.img   # 然后按顺序刷 part2、part3……
fastboot reboot
```
`userdata` 的几个文件是同一个 sparse 镜像拆开的（每个文件只写自己那一段，
其余部分不动）。`SHA256SUMS` 里是校验值。插着 USB 线时平板也是一个网卡：
`ssh alarm@172.16.42.1`（密码 `alarm`，仅限 USB；Wi-Fi 上的 SSH 只接受密钥）。

## 小技巧

- **Firefox 看视频：** Firefox 默认用 CPU 播放视频。想用硬件解码（省很多电），
  打开终端输入 `firefox-hw-video on`，然后重启 Firefox（`firefox-hw-video off`
  可以关掉）。这会关闭 Firefox 视频进程的沙盒，安全性稍低一点。
  看 YouTube 的话再装一个 *h264ify* 扩展。
- **省电：** 屏幕背光最耗电；用 60 Hz 代替 120/144 Hz 能省约 180 mA。
  不用的时候按一下电源键锁屏。
- **旋转锁定：** 在 GNOME 的快捷设置里。

## 自己编译

所有东西都在 Docker 容器里从源码编译（在 Apple Silicon / arm64 上原生运行），
厂商固件和模块从官方线刷包里提取：

```sh
./docker/build-image.sh
docker run --rm -it --privileged --platform linux/arm64 \
    -v "$PWD:/port" -v yupei-work:/port/work yupei-build-arm64 \
    ./build.sh all /port/yupei_images_OS4.0.5.0.XPZCNXM.tgz
```

详细的移植笔记（30 多个设备相关的坑以及解决方法，英文）在
**[docs/PORTING.md](docs/PORTING.md)**。

## 致谢

移植：**koryo**。基于 Arch Linux ARM、Mesa/Freedreno 开发者、AudioReach 和
linux-msm 项目、postmarketOS（libssc、qbootctl、pd-mapper）以及小米公开的内核源码。

厂商固件、DSP 模块和校准文件属于小米/高通，从官方线刷包中提取。
