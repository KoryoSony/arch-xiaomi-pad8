// yupei-bootlog: boot screen on the yupei panel.
//
// GKI has no VT/fbcon, so nothing else draws during boot. By default this
// shows a splash: the Arch Linux logo with three breathing dots. Holding
// Volume Up (any time during boot), "-v", or a splash that is still up after
// SPLASH_TIMEOUT_S switch to the boot log: /dev/kmsg (including journald's
// forwarded systemd messages) in the kernel's 8x16 console font at 2x.
// Drawn on two side-by-side planes like the presenters (the panel needs one
// plane per DSI half). Started by the initramfs once the display driver is
// loaded; the desktop session stops it (SIGTERM) before taking over.
//
// Build: gcc -O2 -static -o yupei-bootlog yupei-bootlog.c -lm
//        (needs font8x16.h from gen-font.py and logo.h from gen-logo.py)
#include "font8x16.h"   // generated from the kernel's lib/fonts/font_8x16.c
#include "logo.h"       // Arch Linux logo, generated from the official SVG
#include <dirent.h>
#include <linux/input.h>
#include <math.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define U64(p) ((uint64_t)(uintptr_t)(p))
#define SCALE 2
#define GW (8 * SCALE)
#define GH (16 * SCALE)

static int drm;
static uint32_t crtc_id, conn_id, planes[2], mode_blob;
static struct drm_mode_modeinfo mode;
static struct { uint32_t fb, pitch; uint8_t *map; } scan[2];
static int front = -1;

static int kio(unsigned long req, void *arg)
{
	return ioctl(drm, req, arg);
}

static uint32_t prop(uint32_t obj, uint32_t type, const char *name)
{
	struct drm_mode_obj_get_properties gp = { .obj_id = obj, .obj_type = type };
	kio(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp);
	uint32_t ids[256];
	uint64_t vals[256];
	if (gp.count_props > 256) gp.count_props = 256;
	gp.props_ptr = U64(ids);
	gp.prop_values_ptr = U64(vals);
	kio(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp);
	for (uint32_t i = 0; i < gp.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = ids[i] };
		kio(DRM_IOCTL_MODE_GETPROPERTY, &p);
		if (!strcmp(p.name, name))
			return ids[i];
	}
	return 0;
}

// Returns 0 once the panel is set up; the display driver may still be probing.
static int kms_init(void)
{
	drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm < 0)
		return -1;
	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
	kio(DRM_IOCTL_SET_CLIENT_CAP, &cap);
	cap = (struct drm_set_client_cap){ DRM_CLIENT_CAP_ATOMIC, 1 };
	if (kio(DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0)
		goto fail;

	struct drm_mode_card_res res = {0};
	if (kio(DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.count_connectors)
		goto fail;
	uint32_t crtcs[16], conns[16], encs[16];
	res.count_fbs = 0;
	if (res.count_crtcs > 16) res.count_crtcs = 16;
	if (res.count_connectors > 16) res.count_connectors = 16;
	if (res.count_encoders > 16) res.count_encoders = 16;
	res.crtc_id_ptr = U64(crtcs);
	res.connector_id_ptr = U64(conns);
	res.encoder_id_ptr = U64(encs);
	kio(DRM_IOCTL_MODE_GETRESOURCES, &res);

	uint32_t enc_id = 0;
	for (uint32_t i = 0; i < res.count_connectors && !conn_id; i++) {
		struct drm_mode_get_connector c = { .connector_id = conns[i] };
		kio(DRM_IOCTL_MODE_GETCONNECTOR, &c);
		if (c.connector_type != DRM_MODE_CONNECTOR_DSI || c.connection != 1 || !c.count_modes)
			continue;
		struct drm_mode_modeinfo modes[32];
		uint32_t e[16];
		if (c.count_modes > 32) c.count_modes = 32;
		if (c.count_encoders > 16) c.count_encoders = 16;
		c.count_props = 0;
		c.modes_ptr = U64(modes);
		c.encoders_ptr = U64(e);
		kio(DRM_IOCTL_MODE_GETCONNECTOR, &c);
		mode = modes[0];
		for (uint32_t m = 0; m < c.count_modes; m++)
			if (modes[m].type & DRM_MODE_TYPE_PREFERRED)
				mode = modes[m];
		conn_id = conns[i];
		enc_id = e[0];
	}
	if (!conn_id)
		goto fail;
	struct drm_mode_get_encoder enc = { .encoder_id = enc_id };
	kio(DRM_IOCTL_MODE_GETENCODER, &enc);
	if (!enc.possible_crtcs)
		goto fail;
	int crtc_idx = __builtin_ctz(enc.possible_crtcs);
	crtc_id = crtcs[crtc_idx];

	struct drm_mode_get_plane_res pr = {0};
	kio(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
	uint32_t pids[64];
	if (pr.count_planes > 64) pr.count_planes = 64;
	pr.plane_id_ptr = U64(pids);
	kio(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
	int np = 0;
	for (uint32_t i = 0; i < pr.count_planes && np < 2; i++) {
		struct drm_mode_get_plane gp = { .plane_id = pids[i] };
		kio(DRM_IOCTL_MODE_GETPLANE, &gp);
		if (gp.possible_crtcs & (1u << crtc_idx))
			planes[np++] = pids[i];
	}
	if (np < 2)
		goto fail;

	for (int i = 0; i < 2; i++) {
		struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
		if (kio(DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
			goto fail;
		struct drm_mode_fb_cmd2 fb = { .width = cd.width, .height = cd.height,
					       .pixel_format = DRM_FORMAT_XRGB8888 };
		fb.handles[0] = cd.handle;
		fb.pitches[0] = cd.pitch;
		struct drm_mode_map_dumb md = { .handle = cd.handle };
		if (kio(DRM_IOCTL_MODE_ADDFB2, &fb) < 0 || kio(DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
			goto fail;
		scan[i].map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, md.offset);
		if (scan[i].map == MAP_FAILED)
			goto fail;
		scan[i].fb = fb.fb_id;
		scan[i].pitch = cd.pitch;
	}
	struct drm_mode_create_blob mb = { .data = U64(&mode), .length = sizeof mode };
	if (kio(DRM_IOCTL_MODE_CREATEPROPBLOB, &mb) < 0)
		goto fail;
	mode_blob = mb.blob_id;
	return 0;
fail:
	close(drm);
	conn_id = 0;
	return -1;
}

static int kms_show(int idx)
{
	uint32_t objs[4], counts[4], props[32];
	uint64_t vals[32];
	int no = 0, k = 0, modeset = front < 0;
	uint32_t w = mode.hdisplay / 2, h = mode.vdisplay;
	if (modeset) {
		objs[no] = conn_id; counts[no++] = 1;
		props[k] = prop(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"); vals[k++] = crtc_id;
		objs[no] = crtc_id; counts[no++] = 2;
		props[k] = prop(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"); vals[k++] = mode_blob;
		props[k] = prop(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"); vals[k++] = 1;
	}
	for (int i = 0; i < 2; i++) {
		uint32_t p = planes[i], t = DRM_MODE_OBJECT_PLANE;
		objs[no] = p;
		counts[no++] = modeset ? 10 : 1;
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
	}
	struct drm_mode_atomic at = {
		.flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0,
		.count_objs = no, .objs_ptr = U64(objs), .count_props_ptr = U64(counts),
		.props_ptr = U64(props), .prop_values_ptr = U64(vals),
	};
	if (kio(DRM_IOCTL_MODE_ATOMIC, &at) < 0)
		return -1;
	front = idx;
	return 0;
}

/* ------------------------------------------------------------ drawing */

#define MAXLINES 256
#define LINELEN 512
static char lines[MAXLINES][LINELEN];
static uint32_t colors[MAXLINES];
static int nlines;

static void add_line(const char *text, uint32_t color)
{
	if (nlines == MAXLINES) {
		memmove(lines, lines[1], sizeof lines[0] * (MAXLINES - 1));
		memmove(colors, colors + 1, sizeof colors[0] * (MAXLINES - 1));
		nlines--;
	}
	snprintf(lines[nlines], LINELEN, "%s", text);
	colors[nlines++] = color;
}

static void draw_text(uint8_t *map, uint32_t pitch, int col, int row, const char *s, uint32_t fg)
{
	int cols = mode.hdisplay / GW;
	for (; *s && col < cols; s++, col++) {
		const unsigned char *g = font8x16 + 16 * (unsigned char)*s;
		for (int y = 0; y < GH; y++) {
			uint32_t *px = (uint32_t *)(map + (size_t)(row * GH + y) * pitch) + col * GW;
			unsigned char bits = g[y / SCALE];
			for (int x = 0; x < GW; x++)
				if (bits & (0x80 >> (x / SCALE)))
					px[x] = fg;
		}
	}
}

static void redraw(void)
{
	int back = front ^ 1;
	if (back < 0)
		back = 0;
	uint8_t *map = scan[back].map;
	uint32_t pitch = scan[back].pitch;
	memset(map, 0, (size_t)pitch * mode.vdisplay);
	int rows = mode.vdisplay / GH;
	draw_text(map, pitch, 0, 0, "Arch Linux ARM on Xiaomi Pad 8 (yupei) - booting", 0x1793d1);
	int first = nlines > rows - 2 ? nlines - (rows - 2) : 0;
	for (int i = first; i < nlines; i++)
		draw_text(map, pitch, 0, 2 + i - first, lines[i], colors[i]);
	kms_show(back);
}

static volatile sig_atomic_t quit;
static void on_term(int s) { quit = 1; }

/* ------------------------------------------------------------ splash */

#define SPLASH_TIMEOUT_S 150
#define DOT_R 14
#define DOT_GAP 64

static int logo_x, logo_y, dots_y;

static void splash_base(int idx)
{
	uint8_t *map = scan[idx].map;
	uint32_t pitch = scan[idx].pitch;
	memset(map, 0, (size_t)pitch * mode.vdisplay);
	logo_x = (mode.hdisplay - LOGO_W) / 2;
	logo_y = mode.vdisplay * 2 / 5 - LOGO_H / 2;
	dots_y = logo_y + LOGO_H + 180;
	for (int y = 0; y < LOGO_H; y++) {
		uint32_t *px = (uint32_t *)(map + (size_t)(logo_y + y) * pitch) + logo_x;
		const unsigned char *src = logo_rgb + (size_t)y * LOGO_W * 3;
		for (int x = 0; x < LOGO_W; x++, src += 3)
			px[x] = src[0] << 16 | src[1] << 8 | src[2];
	}
}

// Three dots breathing in turn, in Arch blue (#1793d1), anti-aliased.
static void splash_frame(int idx, double t)
{
	uint8_t *map = scan[idx].map;
	uint32_t pitch = scan[idx].pitch;
	int cx0 = mode.hdisplay / 2 - DOT_GAP;
	for (int i = 0; i < 3; i++) {
		double v = 0.5 - 0.5 * cos(2 * M_PI * (t / 1.4 - i / 6.0));
		double lvl = 0.12 + 0.88 * v * v;
		int cx = cx0 + i * DOT_GAP;
		for (int y = -DOT_R - 1; y <= DOT_R + 1; y++) {
			uint32_t *px = (uint32_t *)(map + (size_t)(dots_y + y) * pitch);
			for (int x = -DOT_R - 1; x <= DOT_R + 1; x++) {
				double cov = DOT_R + 0.5 - sqrt(x * x + y * y);
				cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
				double a = cov * lvl;
				px[cx + x] = (uint32_t)(0x17 * a) << 16 | (uint32_t)(0x93 * a) << 8 |
					     (uint32_t)(0xd1 * a);
			}
		}
	}
}

/* ------------------------------------------------------------ Volume Up */

#define MAXIN 16
static int infd[MAXIN], nin;

static int has_volup(int f)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
	if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0)
		return 0;
	return bits[KEY_VOLUMEUP / (8 * sizeof(long))] >> (KEY_VOLUMEUP % (8 * sizeof(long))) & 1;
}

static void scan_inputs(void)
{
	for (int i = 0; i < nin; i++)
		close(infd[i]);
	nin = 0;
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && (e = readdir(d)) && nin < MAXIN) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		char p[300];
		snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
		int f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (f < 0)
			continue;
		if (has_volup(f))
			infd[nin++] = f;
		else
			close(f);
	}
	if (d)
		closedir(d);
}

// Held now, or pressed since the last call.
static int volup(void)
{
	int hit = 0;
	for (int i = 0; i < nin; i++) {
		unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
		if (ioctl(infd[i], EVIOCGKEY(sizeof keys), keys) >= 0 &&
		    (keys[KEY_VOLUMEUP / (8 * sizeof(long))] >> (KEY_VOLUMEUP % (8 * sizeof(long))) & 1))
			hit = 1;
		struct input_event ev;
		while (read(infd[i], &ev, sizeof ev) == sizeof ev)
			if (ev.type == EV_KEY && ev.code == KEY_VOLUMEUP && ev.value)
				hit = 1;
	}
	return hit;
}

static double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int verbose = argc > 1 && !strcmp(argv[1], "-v");
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	// The display driver may still be probing when we are started.
	for (int i = 0; kms_init() < 0; i++) {
		if (i > 300)
			return 1;
		usleep(100000);
	}
	int kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	char rec[2048];
	struct timespec last = {0};
	int dirty = 1, splash = !verbose;
	double t0 = mono(), last_scan = -10;
	if (splash) {
		splash_base(0);
		splash_base(1);
	}
	while (!quit) {
		ssize_t n;
		while ((n = read(kmsg, rec, sizeof rec - 1)) > 0) {
			rec[n] = 0;
			// "prio,seq,usec,flags;message\n" (+ continuation lines)
			int prio = atoi(rec) & 7;
			unsigned long long usec = 0;
			char *semi = strchr(rec, ';'), *nl;
			sscanf(rec, "%*d,%*u,%llu", &usec);
			if (!semi)
				continue;
			if ((nl = strchr(semi + 1, '\n')))
				*nl = 0;
			// The status LED driver logs every brightness step (yupei-led).
			if (strstr(semi + 1, "LPG calc_duty"))
				continue;
			char line[LINELEN];
			snprintf(line, sizeof line, "[%5llu.%06llu] %s", usec / 1000000, usec % 1000000, semi + 1);
			add_line(line, prio <= 3 ? 0xff5555 : prio == 4 ? 0xffd75f : 0xc0c0c0);
			dirty = 1;
		}
		if (n < 0 && errno == EPIPE)   // overwritten records: keep reading
			continue;
		double t = mono();
		if (splash) {
			// Input devices appear while modules load: rescan now and then.
			if (t - last_scan > 1) {
				scan_inputs();
				last_scan = t;
			}
			if (volup() || t - t0 > SPLASH_TIMEOUT_S) {
				splash = 0;
				dirty = 1;
				continue;
			}
			int back = front < 0 ? 0 : front ^ 1;
			splash_frame(back, t - t0);
			kms_show(back);   // waits for the flip: paces the animation
			usleep(20000);
			continue;
		}
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
		if (dirty && ms >= 100) {   // at most 10 redraws per second
			redraw();
			last = now;
			dirty = 0;
		}
		struct pollfd p = { .fd = kmsg, .events = POLLIN };
		poll(&p, 1, 100);
	}
	return 0;
}
