// yupei-present-gnome: show a headless GNOME Shell on the yupei panel.
//
// GNOME Shell runs with mutter's headless backend. This client asks it (over
// D-Bus, the RemoteDesktop + ScreenCast APIs gnome-remote-desktop uses) for a
// virtual monitor the size of the panel (created by gnome-shell at startup),
// receives its frames over PipeWire,
// and shows them as two side-by-side planes (the panel needs one plane per
// DSI half). Touches from the panel's touchscreen are forwarded back as
// RemoteDesktop touch events, so GNOME behaves as on a touch device.
//
// Touch mapping (the digitizer is portrait, the panel landscape) can be
// adjusted with YUPEI_TOUCH_SWAP=0/1, YUPEI_TOUCH_INVERT_X=1, YUPEI_TOUCH_INVERT_Y=1.
//
// Build: gcc -O2 yupei-present-gnome.c $(pkg-config --cflags --libs gio-2.0 libpipewire-0.3) -lpthread
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <linux/input.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <pthread.h>
#include <spa/param/video/format-utils.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define U64(p) ((uint64_t)(uintptr_t)(p))

static void die(const char *what)
{
	fprintf(stderr, "yupei-present-gnome: %s: %s\n", what, strerror(errno));
	exit(1);
}

/* ---------------------------------------------------------------- KMS */

static int drm;
static uint32_t crtc_id, conn_id, planes[2], mode_blob;
static struct drm_mode_modeinfo mode;
struct dumb { uint32_t fb, pitch; uint8_t *map; };
static struct dumb scan[2];
static int front = -1;

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
	fprintf(stderr, "yupei-present-gnome: object %u has no property %s\n", obj, name);
	exit(1);
}

static void kms_init(void)
{
	drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm < 0)
		die("open /dev/dri/card0");
	if (ioctl(drm, DRM_IOCTL_SET_MASTER, 0) < 0)
		die("DRM master (is another compositor running?)");
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
		// Refresh rate: YUPEI_REFRESH (Hz) if the panel has that mode at the
		// preferred size, otherwise the preferred mode (120 Hz on yupei).
		const char *hz = getenv("YUPEI_REFRESH");
		for (uint32_t m = 0; hz && m < c.count_modes; m++)
			if (modes[m].hdisplay == mode.hdisplay && modes[m].vdisplay == mode.vdisplay &&
			    modes[m].vrefresh == (uint32_t)atoi(hz)) {
				mode = modes[m];
				break;
			}
		conn_id = conns[i];
		enc_id = e[0];
	}
	if (!conn_id) {
		fprintf(stderr, "yupei-present-gnome: no connected DSI panel\n");
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
		scan[i] = (struct dumb){ fb.fb_id, cd.pitch, map };
	}
	struct drm_mode_create_blob mb = { .data = U64(&mode), .length = sizeof mode };
	kio(DRM_IOCTL_MODE_CREATEPROPBLOB, &mb, "CREATEPROPBLOB");
	mode_blob = mb.blob_id;
}

static void kms_show(int idx)
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
	kio(DRM_IOCTL_MODE_ATOMIC, &at, "ATOMIC commit");
	front = idx;
}

/* -------------------------------------------------------- GNOME D-Bus */

static GDBusConnection *bus;
static char *rd_session, *sc_stream;
static uint32_t node_id;

static GVariant *call(const char *name, const char *path, const char *iface,
		      const char *method, GVariant *args, const char *reply)
{
	GError *err = NULL;
	GVariant *r = g_dbus_connection_call_sync(bus, name, path, iface, method, args,
						   reply ? G_VARIANT_TYPE(reply) : NULL,
						   G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
	if (!r) {
		fprintf(stderr, "yupei-present-gnome: %s.%s: %s\n", iface, method, err->message);
		exit(1);
	}
	return r;
}

static void on_session_closed(GDBusConnection *c, const char *sender, const char *path,
			      const char *iface, const char *signal, GVariant *params, gpointer d)
{
	fprintf(stderr, "yupei-present-gnome: remote desktop session closed, exiting\n");
	exit(2);
}

static void on_stream_added(GDBusConnection *c, const char *sender, const char *path,
			    const char *iface, const char *signal, GVariant *params, gpointer d)
{
	g_variant_get(params, "(u)", &node_id);
}

static void gnome_init(void)
{
	GError *err = NULL;
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!bus) {
		fprintf(stderr, "yupei-present-gnome: session bus: %s\n", err->message);
		exit(1);
	}
	// We start before GNOME (so we, not mutter, become DRM master): wait for it.
	for (int i = 0;; i++) {
		GVariant *o = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", "org.gnome.Mutter.ScreenCast"),
			G_VARIANT_TYPE("(b)"), 0, -1, NULL, NULL);
		gboolean up = FALSE;
		if (o)
			g_variant_get(o, "(b)", &up);
		if (up)
			break;
		if (i > 900) {
			fprintf(stderr, "yupei-present-gnome: GNOME Shell did not appear on the bus\n");
			exit(1);
		}
		usleep(100000);
	}
	GVariant *r = call("org.gnome.Mutter.RemoteDesktop", "/org/gnome/Mutter/RemoteDesktop",
			   "org.gnome.Mutter.RemoteDesktop", "CreateSession", NULL, "(o)");
	g_variant_get(r, "(o)", &rd_session);
	r = call("org.gnome.Mutter.RemoteDesktop", rd_session, "org.freedesktop.DBus.Properties", "Get",
		 g_variant_new("(ss)", "org.gnome.Mutter.RemoteDesktop.Session", "SessionId"), "(v)");
	GVariant *v;
	g_variant_get(r, "(v)", &v);
	const char *session_id = g_variant_get_string(v, NULL);

	GVariantBuilder opts;
	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&opts, "{sv}", "remote-desktop-session-id", g_variant_new_string(session_id));
	r = call("org.gnome.Mutter.ScreenCast", "/org/gnome/Mutter/ScreenCast",
		 "org.gnome.Mutter.ScreenCast", "CreateSession", g_variant_new("(a{sv})", &opts), "(o)");
	char *sc_session;
	g_variant_get(r, "(o)", &sc_session);

	// Record the virtual monitor gnome-shell created at startup
	// (--virtual-monitor WxH, connector Meta-0), cursor drawn into frames.
	// Hot-adding one with RecordVirtual makes headless, software-rendered
	// mutter crash or stall when the stream starts.
	const char *connector = getenv("YUPEI_MONITOR") ? getenv("YUPEI_MONITOR") : "Meta-0";
	g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&opts, "{sv}", "cursor-mode", g_variant_new_uint32(1));
	r = call("org.gnome.Mutter.ScreenCast", sc_session, "org.gnome.Mutter.ScreenCast.Session",
		 "RecordMonitor", g_variant_new("(sa{sv})", connector, &opts), "(o)");
	g_variant_get(r, "(o)", &sc_stream);

	g_dbus_connection_signal_subscribe(bus, NULL, "org.gnome.Mutter.ScreenCast.Stream",
					   "PipeWireStreamAdded", sc_stream, NULL,
					   G_DBUS_SIGNAL_FLAGS_NONE, on_stream_added, NULL, NULL);
	g_dbus_connection_signal_subscribe(bus, NULL, "org.gnome.Mutter.RemoteDesktop.Session",
					   "Closed", rd_session, NULL,
					   G_DBUS_SIGNAL_FLAGS_NONE, on_session_closed, NULL, NULL);
	call("org.gnome.Mutter.RemoteDesktop", rd_session, "org.gnome.Mutter.RemoteDesktop.Session",
	     "Start", NULL, NULL);
	for (int i = 0; !node_id && i < 1000; i++)
		g_main_context_iteration(NULL, FALSE), usleep(10000);
	if (!node_id) {
		fprintf(stderr, "yupei-present-gnome: no PipeWire stream from mutter\n");
		exit(1);
	}
	fprintf(stderr, "yupei-present-gnome: remote desktop %s, stream %s, pipewire node %u\n",
		rd_session, sc_stream, node_id);
}

/* -------------------------------------------------------------- touch */

// Prefer yupei-touchd's screen-oriented device; the panel's own
// NVTCapacitiveTouchScreen (portrait axes) only reports in non-THP setups.
static int open_touch(char *name, size_t len, int *oriented)
{
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < 64; i++) {
			char path[64];
			snprintf(path, sizeof path, "/dev/input/event%d", i);
			int f = open(path, O_RDONLY | O_CLOEXEC);
			if (f < 0)
				continue;
			if (ioctl(f, EVIOCGNAME(len), name) >= 0 &&
			    (pass == 0 ? !strcmp(name, "yupei-thp-touch") : strstr(name, "TouchScreen") != NULL)) {
				*oriented = pass == 0;
				return f;
			}
			close(f);
		}
	return -1;
}

static void touch_loop(int fd, const char *name, int oriented);

static void *touch_thread(void *arg)
{
	for (;;) {   // re-open if the device goes away (e.g. yupei-touchd restarts)
		char name[128] = "";
		int oriented = 0, fd = -1;
		for (int tries = 0; tries < 20 && fd < 0; tries++) {   // give yupei-touchd a moment
			fd = open_touch(name, sizeof name, &oriented);
			if (fd >= 0 && !oriented && tries < 19) {
				close(fd);
				fd = -1;
				sleep(1);
			}
		}
		if (fd < 0) {
			sleep(2);
			continue;
		}
		touch_loop(fd, name, oriented);
		close(fd);
		sleep(1);
	}
	return NULL;
}

static void touch_loop(int fd, const char *name, int oriented)
{
	struct input_absinfo ax, ay;
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
	const char *e;
	int swap = (e = getenv("YUPEI_TOUCH_SWAP")) ? atoi(e) : !oriented;
	int inv_x = (e = getenv("YUPEI_TOUCH_INVERT_X")) ? atoi(e) : 0;
	int inv_y = (e = getenv("YUPEI_TOUCH_INVERT_Y")) ? atoi(e) : 0;
	fprintf(stderr, "yupei-present-gnome: touch %s x 0..%d y 0..%d swap=%d invx=%d invy=%d\n",
		name, ax.maximum, ay.maximum, swap, inv_x, inv_y);
	if (oriented)
		inv_x = inv_y = 0;

	enum { MAXS = 16 };
	struct { int id, x, y, dirty, down; } s[MAXS];
	memset(s, 0, sizeof s);
	for (int i = 0; i < MAXS; i++)
		s[i].id = -1;
	int slot = 0;
	const char *ifc = "org.gnome.Mutter.RemoteDesktop.Session", *svc = "org.gnome.Mutter.RemoteDesktop";
	struct input_event ev;
	while (read(fd, &ev, sizeof ev) == sizeof ev) {
		if (ev.type == EV_ABS) {
			if (ev.code == ABS_MT_SLOT)
				slot = ev.value < MAXS ? ev.value : MAXS - 1;
			else if (ev.code == ABS_MT_TRACKING_ID)
				s[slot].id = ev.value, s[slot].dirty = 1;
			else if (ev.code == ABS_MT_POSITION_X)
				s[slot].x = ev.value, s[slot].dirty = 1;
			else if (ev.code == ABS_MT_POSITION_Y)
				s[slot].y = ev.value, s[slot].dirty = 1;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			for (int i = 0; i < MAXS; i++) {
				if (!s[i].dirty)
					continue;
				s[i].dirty = 0;
				double fx = (double)s[i].x / (ax.maximum + 1), fy = (double)s[i].y / (ay.maximum + 1);
				double u = swap ? fy : fx, v = swap ? fx : fy;
				if (inv_x) u = 1 - u;
				if (inv_y) v = 1 - v;
				double px = u * mode.hdisplay, py = v * mode.vdisplay;
				if (s[i].id >= 0 && !s[i].down) {
					s[i].down = 1;
					g_dbus_connection_call(bus, svc, rd_session, ifc, "NotifyTouchDown",
							       g_variant_new("(sudd)", sc_stream, i, px, py),
							       NULL, 0, -1, NULL, NULL, NULL);
				} else if (s[i].id >= 0) {
					g_dbus_connection_call(bus, svc, rd_session, ifc, "NotifyTouchMotion",
							       g_variant_new("(sudd)", sc_stream, i, px, py),
							       NULL, 0, -1, NULL, NULL, NULL);
				} else if (s[i].down) {
					s[i].down = 0;
					g_dbus_connection_call(bus, svc, rd_session, ifc, "NotifyTouchUp",
							       g_variant_new("(u)", i), NULL, 0, -1, NULL, NULL, NULL);
				}
			}
			g_dbus_connection_flush_sync(bus, NULL, NULL);
		}
	}
	// Device gone: release any fingers still down.
	for (int i = 0; i < MAXS; i++)
		if (s[i].down)
			g_dbus_connection_call(bus, svc, rd_session, ifc, "NotifyTouchUp",
					       g_variant_new("(u)", i), NULL, 0, -1, NULL, NULL, NULL);
}

/* ------------------------------------------------- mouse and keyboard */

// Headless mutter ignores physical input devices: forward mice and keyboards
// (anything with relative axes or letter keys) as RemoteDesktop events.
// Devices are rescanned every 2 s so hot-plugged ones are picked up.
#define BITS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))
static int test_bit(const unsigned long *b, int n) { return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }

static void rd_call(const char *method, GVariant *args)
{
	g_dbus_connection_call(bus, "org.gnome.Mutter.RemoteDesktop", rd_session,
			       "org.gnome.Mutter.RemoteDesktop.Session", method, args,
			       NULL, 0, -1, NULL, NULL, NULL);
}

static void *input_thread(void *arg)
{
	enum { MAXDEV = 32 };
	int fds[MAXDEV];
	char paths[MAXDEV][32];
	int n = 0;
	for (;;) {
		// (Re)scan for new devices.
		for (int i = 0; i < 64; i++) {
			char p[32], name[128] = "";
			snprintf(p, sizeof p, "/dev/input/event%d", i);
			int known = 0;
			for (int k = 0; k < n; k++)
				if (!strcmp(paths[k], p)) known = 1;
			if (known || n == MAXDEV)
				continue;
			int fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
			if (fd < 0)
				continue;
			unsigned long ev[BITS(EV_MAX + 1)] = {0}, keys[BITS(KEY_MAX + 1)] = {0}, rel[BITS(REL_MAX + 1)] = {0};
			ioctl(fd, EVIOCGNAME(sizeof name), name);
			ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
			ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
			int mouse = test_bit(ev, EV_REL) && test_bit(rel, REL_X) && test_bit(keys, BTN_LEFT);
			int kbd = test_bit(ev, EV_KEY) && test_bit(keys, KEY_A) && test_bit(keys, KEY_Z);
			if (!mouse && !kbd) {
				close(fd);
				continue;
			}
			fprintf(stderr, "yupei-present-gnome: input %s: %s (%s)\n", p, name,
				mouse && kbd ? "mouse+keyboard" : mouse ? "mouse" : "keyboard");
			strcpy(paths[n], p);
			fds[n++] = fd;
		}
		// Forward events for ~2 s, then rescan.
		struct pollfd pfd[MAXDEV];
		for (int k = 0; k < n; k++)
			pfd[k] = (struct pollfd){ .fd = fds[k], .events = POLLIN };
		for (int t = 0; t < 20; t++) {
			if (poll(pfd, n, 100) <= 0)
				continue;
			int dx = 0, dy = 0;
			for (int k = 0; k < n; k++) {
				if (pfd[k].revents & (POLLERR | POLLHUP)) {   // unplugged
					close(fds[k]);
					if (k != n - 1) {
						fds[k] = fds[n - 1];
						memcpy(paths[k], paths[n - 1], sizeof paths[k]);
						pfd[k] = pfd[n - 1];
					}
					n--; k--;
					continue;
				}
				struct input_event e;
				while (read(fds[k], &e, sizeof e) == sizeof e) {
					if (e.type == EV_REL && e.code == REL_X)
						dx += e.value;
					else if (e.type == EV_REL && e.code == REL_Y)
						dy += e.value;
					else if (e.type == EV_REL && e.code == REL_WHEEL)
						rd_call("NotifyPointerAxisDiscrete", g_variant_new("(ui)", 0, -e.value));
					else if (e.type == EV_REL && e.code == REL_HWHEEL)
						rd_call("NotifyPointerAxisDiscrete", g_variant_new("(ui)", 1, e.value));
					else if (e.type == EV_KEY && e.value != 2 && e.code >= BTN_MOUSE && e.code < BTN_JOYSTICK)
						rd_call("NotifyPointerButton", g_variant_new("(ib)", e.code, e.value));
					else if (e.type == EV_KEY && e.value != 2 && e.code < BTN_MISC)
						rd_call("NotifyKeyboardKeycode", g_variant_new("(ub)", e.code, e.value));
				}
			}
			if (dx || dy)
				rd_call("NotifyPointerMotionRelative", g_variant_new("(dd)", (double)dx, (double)dy));
			g_dbus_connection_flush_sync(bus, NULL, NULL);
		}
	}
	return NULL;
}

/* ----------------------------------------------------------- PipeWire */

static struct pw_stream *stream;
static struct pw_core *core;
static struct spa_video_info_raw info;

static void on_param_changed(void *d, uint32_t id, const struct spa_pod *param)
{
	if (!param || id != SPA_PARAM_Format)
		return;
	spa_format_video_raw_parse(param, &info);
	fprintf(stderr, "yupei-present-gnome: stream %ux%u format %u\n",
		info.size.width, info.size.height, info.format);
	uint8_t buf[256];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *p = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	pw_stream_update_params(stream, &p, 1);
}

static void on_process(void *d)
{
	struct pw_buffer *b = NULL, *t;
	while ((t = pw_stream_dequeue_buffer(stream))) {   // keep only the newest frame
		if (b)
			pw_stream_queue_buffer(stream, b);
		b = t;
	}
	if (!b)
		return;
	struct spa_data *sd = &b->buffer->datas[0];
	static unsigned frames;
	if (frames++ < 5 || frames % 600 == 0)
		fprintf(stderr, "yupei-present-gnome: frame %u type=%u data=%p size=%u stride=%d flags=0x%x\n",
			frames, sd->type, sd->data, sd->chunk->size, sd->chunk->stride, sd->chunk->flags);
	if (sd->data && sd->chunk->size && !(sd->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
		uint8_t *src = (uint8_t *)sd->data + sd->chunk->offset;
		int stride = sd->chunk->stride ? sd->chunk->stride : (int)info.size.width * 4;
		struct dumb *dst = &scan[front ^ 1];
		uint32_t w = info.size.width < mode.hdisplay ? info.size.width : mode.hdisplay;
		uint32_t h = info.size.height < mode.vdisplay ? info.size.height : mode.vdisplay;
		for (uint32_t y = 0; y < h; y++)
			memcpy(dst->map + (size_t)y * dst->pitch, src + (size_t)y * stride, w * 4);
		kms_show(front ^ 1);
		// Frames shown per second, logged every 10 s.
		static unsigned shown;
		static struct timespec t0;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (!t0.tv_sec)
			t0 = now;
		shown++;
		double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
		if (el >= 10) {
			fprintf(stderr, "yupei-present-gnome: %.1f fps\n", shown / el);
			shown = 0;
			t0 = now;
		}
	}
	pw_stream_queue_buffer(stream, b);
}

static int was_streaming;

// If the cast is stopped (e.g. "Stop" in GNOME's screen-sharing menu) or the
// stream breaks, exit; the session script restarts us with a new session.
static void on_state_changed(void *d, enum pw_stream_state old, enum pw_stream_state state, const char *err)
{
	fprintf(stderr, "yupei-present-gnome: stream state %s%s%s\n", pw_stream_state_as_string(state),
		err ? ": " : "", err ? err : "");
	if (state == PW_STREAM_STATE_STREAMING)
		was_streaming = 1;
	if (state == PW_STREAM_STATE_ERROR || (was_streaming && state == PW_STREAM_STATE_UNCONNECTED))
		exit(2);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

// D-Bus signals (session Closed) are dispatched on GLib's default context,
// while the main thread runs PipeWire's loop.
static void *glib_thread(void *arg)
{
	g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return NULL;
}

int main(int argc, char **argv)
{
	kms_init();
	fprintf(stderr, "yupei-present-gnome: panel mode %s %ux%u@%u\n", mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);
	kms_show(0);
	gnome_init();
	pthread_t th;
	pthread_create(&th, NULL, touch_thread, NULL);
	pthread_t th2, th3;
	pthread_create(&th2, NULL, input_thread, NULL);
	pthread_create(&th3, NULL, glib_thread, NULL);

	pw_init(&argc, &argv);
	struct pw_main_loop *loop = pw_main_loop_new(NULL);
	struct pw_context *ctx = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	core = pw_context_connect(ctx, NULL, 0);
	if (!core)
		die("pipewire connect");
	// Current PipeWire ignores the id passed to pw_stream_connect(); the session
	// manager links by target.object (mutter's node id is its object.serial).
	char target[16];
	snprintf(target, sizeof target, "%u", node_id);
	stream = pw_stream_new(core, "yupei-present-gnome",
			       pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
						 PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_TARGET_OBJECT, target, NULL));
	struct spa_hook hook;
	pw_stream_add_listener(stream, &hook, &stream_events, NULL);

	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[1];
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx,
							       SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(mode.hdisplay, mode.vdisplay)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(mode.vrefresh, 1),
									  &SPA_FRACTION(0, 1),
									  &SPA_FRACTION(240, 1)));
	if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0)
		die("pw_stream_connect");
	pw_main_loop_run(loop);
	return 0;
}
