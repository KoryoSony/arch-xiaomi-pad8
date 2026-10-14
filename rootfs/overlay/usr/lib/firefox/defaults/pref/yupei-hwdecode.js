// Xiaomi Pad 8: hardware video decoding through the Qualcomm V4L2 decoder
// (/dev/video32, msm_vidc) with our patched system FFmpeg (ffmpeg/): H.264
// and HEVC. Firefox's bundled ffvpx (VP9/AV1) has no dma-buf support for
// this driver, so its hardware attempt is turned off (software VP9/AV1;
// the h264ify extension makes YouTube send H.264).
pref("media.hardware-video-decoding.force-enabled", true);
pref("media.ffvpx-hw.enabled", false);
// V4L2 frames are shown zero-copy as dma-bufs: needs GPU WebRender.
pref("gfx.webrender.all", true);
pref("widget.dmabuf.force-enabled", true);
