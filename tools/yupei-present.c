// yupei-present: put a Wayland compositor's output on the yupei panel.
//
// The panel is driven by Qualcomm's downstream SDE driver as two halves (dual
// DSI + DSC). A single full-width plane underruns, and compositors such as
// sway only ever use one primary plane. So sway runs on wlroots' headless
// backend, and this client copies each damaged frame (wlr-screencopy) into a
// dumb buffer shown as two side-by-side planes via atomic KMS.
//
// Build: gcc -O2 -o yupei-present yupei-present.c wlr-screencopy-protocol.c -lwayland-client
#define _GNU_SOURCE
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define U64(p) ((uint64_t)(uintptr_t)(p))

/* ---------------------------------------------------------------- KMS */

static int drm;
static uint32_t crtc_id, conn_id, planes[2], mode_blob;
static struct drm_mode_modeinfo mode;

struct dumb { uint32_t handle, fb, pitch; uint64_t size; uint8_t *map; };
static struct dumb scan[2];
static int front = -1;

static void die(const char *what)
{
	fprintf(stderr, "yupei-present: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void kio(unsigned long req, void *arg, const char *what)
{
	if (ioctl(drm, req, arg) < 0)
		die(what);
}

static uint32_t prop(uint32_t obj, uint32_t type, const char *name)
{
	struct drm_mode_obj_get_properties gp = { .obj_id = obj, .obj_type = type };
	kio(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp, "OBJ_GETPROPERTIES");
	uint32_t ids[gp.count_props + 1];
	uint64_t vals[gp.count_props + 1];
	gp.props_ptr = U64(ids);
	gp.prop_values_ptr = U64(vals);
	kio(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp, "OBJ_GETPROPERTIES");
	for (uint32_t i = 0; i < gp.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = ids[i] };
		kio(DRM_IOCTL_MODE_GETPROPERTY, &p, "GETPROPERTY");
		if (!strcmp(p.name, name))
			return ids[i];
	}
	fprintf(stderr, "yupei-present: object %u has no property %s\n", obj, name);
	exit(1);
}

static void kms_init(void)
{
	drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm < 0)
		die("open /dev/dri/card0");
	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
	kio(DRM_IOCTL_SET_CLIENT_CAP, &cap, "CAP_UNIVERSAL_PLANES");
	cap = (struct drm_set_client_cap){ DRM_CLIENT_CAP_ATOMIC, 1 };
	kio(DRM_IOCTL_SET_CLIENT_CAP, &cap, "CAP_ATOMIC");

	struct drm_mode_card_res res = {0};
	kio(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	uint32_t crtcs[res.count_crtcs + 1], conns[res.count_connectors + 1], encs[res.count_encoders + 1];
	res.count_fbs = 0;
	res.crtc_id_ptr = U64(crtcs);
	res.connector_id_ptr = U64(conns);
	res.encoder_id_ptr = U64(encs);
	kio(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");

	uint32_t enc_id = 0;
	for (uint32_t i = 0; i < res.count_connectors && !conn_id; i++) {
		struct drm_mode_get_connector c = { .connector_id = conns[i] };
		kio(DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR");
		if (c.connector_type != DRM_MODE_CONNECTOR_DSI || c.connection != 1 || !c.count_modes)
			continue;
		struct drm_mode_modeinfo modes[c.count_modes];
		uint32_t e[c.count_encoders + 1];
		c.count_props = 0;
		c.modes_ptr = U64(modes);
		c.encoders_ptr = U64(e);
		kio(DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR");
		mode = modes[0];
		for (uint32_t m = 0; m < c.count_modes; m++)
			if (modes[m].type & DRM_MODE_TYPE_PREFERRED)
				mode = modes[m];
		conn_id = conns[i];
		enc_id = e[0];
	}
	if (!conn_id) {
		fprintf(stderr, "yupei-present: no connected DSI panel\n");
		exit(1);
	}
	struct drm_mode_get_encoder enc = { .encoder_id = enc_id };
	kio(DRM_IOCTL_MODE_GETENCODER, &enc, "GETENCODER");
	int crtc_idx = __builtin_ctz(enc.possible_crtcs);
	crtc_id = crtcs[crtc_idx];

	struct drm_mode_get_plane_res pr = {0};
	kio(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr, "GETPLANERESOURCES");
	uint32_t pids[pr.count_planes + 1];
	pr.plane_id_ptr = U64(pids);
	kio(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr, "GETPLANERESOURCES");
	int np = 0;
	for (uint32_t i = 0; i < pr.count_planes && np < 2; i++) {
		struct drm_mode_get_plane gp = { .plane_id = pids[i] };
		kio(DRM_IOCTL_MODE_GETPLANE, &gp, "GETPLANE");
		if (gp.possible_crtcs & (1u << crtc_idx))
			planes[np++] = pids[i];
	}
	if (np < 2) {
		fprintf(stderr, "yupei-present: need two planes\n");
		exit(1);
	}

	for (int i = 0; i < 2; i++) {
		struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
		kio(DRM_IOCTL_MODE_CREATE_DUMB, &cd, "CREATE_DUMB");
		struct drm_mode_fb_cmd2 fb = { .width = cd.width, .height = cd.height,
					       .pixel_format = DRM_FORMAT_XRGB8888 };
		fb.handles[0] = cd.handle;
		fb.pitches[0] = cd.pitch;
		kio(DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2");
		struct drm_mode_map_dumb md = { .handle = cd.handle };
		kio(DRM_IOCTL_MODE_MAP_DUMB, &md, "MAP_DUMB");
		uint8_t *map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, md.offset);
		if (map == MAP_FAILED)
			die("mmap dumb");
		memset(map, 0, cd.size);
		scan[i] = (struct dumb){ cd.handle, fb.fb_id, cd.pitch, cd.size, map };
	}

	struct drm_mode_create_blob mb = { .data = U64(&mode), .length = sizeof mode };
	kio(DRM_IOCTL_MODE_CREATEPROPBLOB, &mb, "CREATEPROPBLOB");
	mode_blob = mb.blob_id;
	fprintf(stderr, "yupei-present: panel %ux%u@%u on crtc %u, planes %u+%u\n",
		mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc_id, planes[0], planes[1]);
}

// Show scan[idx] as two half-width planes. The first commit also sets the mode.
static void kms_show(int idx)
{
	uint32_t objs[4], counts[4], props[32];
	uint64_t vals[32];
	int no = 0, k = 0;
	uint32_t w = mode.hdisplay / 2, h = mode.vdisplay;
	int modeset = front < 0;

	if (modeset) {
		objs[no] = conn_id; counts[no] = 1;
		props[k] = prop(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"); vals[k++] = crtc_id;
		no++;
		objs[no] = crtc_id; counts[no] = 2;
		props[k] = prop(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"); vals[k++] = mode_blob;
		props[k] = prop(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"); vals[k++] = 1;
		no++;
	}
	for (int i = 0; i < 2; i++) {
		uint32_t p = planes[i], t = DRM_MODE_OBJECT_PLANE;
		objs[no] = p;
		counts[no] = modeset ? 10 : 1;
		props[k] = prop(p, t, "FB_ID"); vals[k++] = scan[idx].fb;
		if (modeset) {
			props[k] = prop(p, t, "CRTC_ID"); vals[k++] = crtc_id;
			props[k] = prop(p, t, "SRC_X"); vals[k++] = (uint64_t)(i * w) << 16;
			props[k] = prop(p, t, "SRC_Y"); vals[k++] = 0;
			props[k] = prop(p, t, "SRC_W"); vals[k++] = (uint64_t)w << 16;
			props[k] = prop(p, t, "SRC_H"); vals[k++] = (uint64_t)h << 16;
			props[k] = prop(p, t, "CRTC_X"); vals[k++] = i * w;
			props[k] = prop(p, t, "CRTC_Y"); vals[k++] = 0;
			props[k] = prop(p, t, "CRTC_W"); vals[k++] = w;
			props[k] = prop(p, t, "CRTC_H"); vals[k++] = h;
		}
		no++;
	}
	struct drm_mode_atomic at = {
		.flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0,
		.count_objs = no, .objs_ptr = U64(objs), .count_props_ptr = U64(counts),
		.props_ptr = U64(props), .prop_values_ptr = U64(vals),
	};
	kio(DRM_IOCTL_MODE_ATOMIC, &at, "ATOMIC commit");
	front = idx;
}

/* ----------------------------------------------------------- Wayland */

static struct wl_shm *shm;
static struct wl_output *output;
static struct zwlr_screencopy_manager_v1 *copier;
static struct wl_buffer *shm_buf;
static uint8_t *shm_map;
static uint32_t shm_w, shm_h, shm_stride, shm_fmt;
static int frame_done, frame_failed, y_invert;

static void frame_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt,
			 uint32_t w, uint32_t h, uint32_t stride)
{
	if (shm_buf)
		return;
	if (fmt != WL_SHM_FORMAT_XRGB8888 && fmt != WL_SHM_FORMAT_ARGB8888) {
		fprintf(stderr, "yupei-present: unsupported shm format %u\n", fmt);
		exit(1);
	}
	size_t size = (size_t)stride * h;
	int fd = memfd_create("yupei-present", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		die("memfd");
	shm_map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm_map == MAP_FAILED)
		die("mmap shm");
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	shm_buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, fmt);
	wl_shm_pool_destroy(pool);
	close(fd);
	shm_w = w; shm_h = h; shm_stride = stride; shm_fmt = fmt;
	fprintf(stderr, "yupei-present: capturing %ux%u\n", w, h);
}

static void frame_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
	y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns)
{
	frame_done = 1;
}

static void frame_failed_cb(void *d, struct zwlr_screencopy_frame_v1 *f)
{
	frame_failed = 1;
}

static void frame_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}
static void frame_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h) {}
static void frame_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer, .flags = frame_flags, .ready = frame_ready,
	.failed = frame_failed_cb, .damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf, .buffer_done = frame_buffer_done,
};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		copier = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
}

static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { global, global_remove };

// Copy the captured frame into the back scanout buffer, scaling 1:1 (sway's
// headless output is configured to the panel size) and clipping otherwise.
static void blit(struct dumb *dst)
{
	uint32_t w = shm_w < mode.hdisplay ? shm_w : mode.hdisplay;
	uint32_t h = shm_h < mode.vdisplay ? shm_h : mode.vdisplay;
	for (uint32_t y = 0; y < h; y++) {
		uint32_t sy = y_invert ? shm_h - 1 - y : y;
		memcpy(dst->map + (size_t)y * dst->pitch, shm_map + (size_t)sy * shm_stride, w * 4);
	}
}

int main(void)
{
	kms_init();
	kms_show(0);   // black screen, takes over from the bootloader splash

	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "yupei-present: cannot connect to Wayland (WAYLAND_DISPLAY?)\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!shm || !output || !copier) {
		fprintf(stderr, "yupei-present: compositor lacks wl_shm/wl_output/wlr-screencopy\n");
		return 1;
	}

	for (int first = 1;; first = 0) {
		struct zwlr_screencopy_frame_v1 *f = zwlr_screencopy_manager_v1_capture_output(copier, 1, output);
		zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, NULL);
		frame_done = frame_failed = 0;
		while (!shm_buf && !frame_failed && wl_display_dispatch(dpy) != -1)
			;
		// copy_with_damage waits until the output changes: no busy polling.
		if (first)
			zwlr_screencopy_frame_v1_copy(f, shm_buf);
		else
			zwlr_screencopy_frame_v1_copy_with_damage(f, shm_buf);
		while (!frame_done && !frame_failed)
			if (wl_display_dispatch(dpy) == -1)
				die("wayland connection lost");
		zwlr_screencopy_frame_v1_destroy(f);
		if (frame_failed)
			continue;
		int back = front ^ 1;
		blit(&scan[back]);
		kms_show(back);
	}
}
