// kmsatomic: atomic KMS test for the yupei panel (Qualcomm SDE, no libdrm).
// The panel is 3200 px wide, wider than one SDE pipe can fetch, so the frame
// is shown as two side-by-side planes (source split), as Android's HWC does.
// Usage: kmsatomic [seconds] [halves] [mode index]   halves=1: single plane.
// Build: gcc -O2 -static -o kmsatomic kmsatomic.c
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

#define U64(p) ((uint64_t)(uintptr_t)(p))
static int fd;

static int xioctl(unsigned long req, void *arg, const char *what, int fatal)
{
	if (ioctl(fd, req, arg) == 0)
		return 0;
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	if (fatal)
		exit(1);
	return -1;
}

// Look up a property id by name on an object.
static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
	struct drm_mode_obj_get_properties gp = { .obj_id = obj, .obj_type = type };
	xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp, "OBJ_GETPROPERTIES", 1);
	uint32_t *ids = calloc(gp.count_props + 1, 4);
	uint64_t *vals = calloc(gp.count_props + 1, 8);
	gp.props_ptr = U64(ids); gp.prop_values_ptr = U64(vals);
	xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp, "OBJ_GETPROPERTIES", 1);
	for (uint32_t i = 0; i < gp.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = ids[i] };
		xioctl(DRM_IOCTL_MODE_GETPROPERTY, &p, "GETPROPERTY", 1);
		if (!strcmp(p.name, name)) {
			if (value) *value = vals[i];
			return ids[i];
		}
	}
	return 0;
}

static void print_blob(uint64_t blob_id, const char *label)
{
	struct drm_mode_get_blob b = { .blob_id = (uint32_t)blob_id };
	if (!blob_id || xioctl(DRM_IOCTL_MODE_GETPROPBLOB, &b, "GETPROPBLOB", 0)) return;
	char *data = calloc(b.length + 1, 1);
	b.data = U64(data);
	xioctl(DRM_IOCTL_MODE_GETPROPBLOB, &b, "GETPROPBLOB", 0);
	for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n"))
		if (strstr(line, "width") || strstr(line, "linewidth") || strstr(line, "pipe"))
			printf("  %s %s\n", label, line);
}

struct req { uint32_t obj, prop; uint64_t val; };
static struct req reqs[64];
static int nreq;
static void add(uint32_t obj, uint32_t prop, uint64_t val, const char *name)
{
	if (!prop) { fprintf(stderr, "missing property %s on %u\n", name, obj); exit(1); }
	reqs[nreq++] = (struct req){ obj, prop, val };
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 20;
	int halves = argc > 2 ? atoi(argv[2]) : 2;
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }
	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
	xioctl(DRM_IOCTL_SET_CLIENT_CAP, &cap, "CAP_UNIVERSAL_PLANES", 1);
	cap = (struct drm_set_client_cap){ DRM_CLIENT_CAP_ATOMIC, 1 };
	xioctl(DRM_IOCTL_SET_CLIENT_CAP, &cap, "CAP_ATOMIC", 1);

	struct drm_mode_card_res res = {0};
	xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES", 1);
	uint32_t crtcs[16], conns[16], encs[16];
	res.count_fbs = 0; res.count_crtcs = res.count_crtcs > 16 ? 16 : res.count_crtcs;
	res.count_connectors = res.count_connectors > 16 ? 16 : res.count_connectors;
	res.count_encoders = res.count_encoders > 16 ? 16 : res.count_encoders;
	res.crtc_id_ptr = U64(crtcs); res.connector_id_ptr = U64(conns); res.encoder_id_ptr = U64(encs);
	xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES", 1);

	// The built-in panel: the connected DSI connector, its preferred mode.
	uint32_t conn_id = 0, enc_id = 0; struct drm_mode_modeinfo mode = {0};
	for (uint32_t i = 0; i < res.count_connectors && !conn_id; i++) {
		struct drm_mode_get_connector c = { .connector_id = conns[i] };
		xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR", 1);
		if (c.connector_type != DRM_MODE_CONNECTOR_DSI || c.connection != 1 || !c.count_modes)
			continue;
		struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof *modes);
		uint32_t e[16];
		c.count_encoders = c.count_encoders > 16 ? 16 : c.count_encoders;
		c.count_props = 0;
		c.modes_ptr = U64(modes); c.encoders_ptr = U64(e);
		xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c, "GETCONNECTOR", 1);
		mode = modes[0];
		for (uint32_t m = 0; m < c.count_modes; m++)
			if (modes[m].type & DRM_MODE_TYPE_PREFERRED) { mode = modes[m]; break; }
		if (argc > 3 && atoi(argv[3]) < (int)c.count_modes)
			mode = modes[atoi(argv[3])];   // kmsatomic SECONDS HALVES MODE_INDEX
		conn_id = conns[i]; enc_id = e[0];
	}
	if (!conn_id) { fprintf(stderr, "no DSI panel\n"); return 1; }
	struct drm_mode_get_encoder enc = { .encoder_id = enc_id };
	xioctl(DRM_IOCTL_MODE_GETENCODER, &enc, "GETENCODER", 1);
	int crtc_idx = -1;
	for (uint32_t c = 0; c < res.count_crtcs; c++)
		if (enc.possible_crtcs & (1u << c)) { crtc_idx = c; break; }
	uint32_t crtc_id = crtcs[crtc_idx];
	printf("panel connector %u, %s %ux%u@%u, crtc %u\n", conn_id, mode.name,
	       mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc_id);

	// Planes usable on that CRTC.
	struct drm_mode_get_plane_res pr = {0};
	xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr, "GETPLANERESOURCES", 1);
	uint32_t *pids = calloc(pr.count_planes, 4);
	pr.plane_id_ptr = U64(pids);
	xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr, "GETPLANERESOURCES", 1);
	uint32_t planes[2]; int np = 0;
	for (uint32_t i = 0; i < pr.count_planes && np < halves; i++) {
		struct drm_mode_get_plane gp = { .plane_id = pids[i] };
		xioctl(DRM_IOCTL_MODE_GETPLANE, &gp, "GETPLANE", 1);
		if (!(gp.possible_crtcs & (1u << crtc_idx))) continue;
		uint64_t capblob = 0;
		prop_id(pids[i], DRM_MODE_OBJECT_PLANE, "capabilities", &capblob);
		char label[32]; snprintf(label, sizeof label, "plane %u:", pids[i]);
		print_blob(capblob, label);
		planes[np++] = pids[i];
	}
	if (np < halves) { fprintf(stderr, "only %d planes for crtc\n", np); return 1; }

	// One full-screen dumb buffer: colour bars + "ARCH" banner.
	struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
	xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd, "CREATE_DUMB", 1);
	struct drm_mode_fb_cmd2 fb = { .width = cd.width, .height = cd.height,
				       .pixel_format = DRM_FORMAT_XRGB8888 };
	fb.handles[0] = cd.handle; fb.pitches[0] = cd.pitch;
	xioctl(DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2", 1);
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	xioctl(DRM_IOCTL_MODE_MAP_DUMB, &md, "MAP_DUMB", 1);
	uint8_t *px = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (px == MAP_FAILED) { perror("mmap"); return 1; }
	static const uint32_t bars[] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
					  0xff00ff, 0xff0000, 0x0000ff, 0x1793d1 };
	static const char *banner[] = { " ### ####   ### #  #", "#  # #   # #    #  #",
					"#### ####  #    ####", "#  # #  #  #    #  #",
					"#  # #   #  ### #  #" };
	int cell = cd.width / 32, bx = (cd.width - 20 * cell) / 2, by = (cd.height - 5 * cell) / 2;
	for (uint32_t y = 0; y < cd.height; y++) {
		uint32_t *row = (uint32_t *)(px + y * cd.pitch);
		for (uint32_t x = 0; x < cd.width; x++) {
			uint32_t c = bars[x * 8 / cd.width];
			int cx = ((int)x - bx) / cell, cy = ((int)y - by) / cell;
			if ((int)x >= bx && (int)y >= by && cx < 20 && cy < 5)
				c = banner[cy][cx] == '#' ? 0xffffff : 0x000000;
			row[x] = c;
		}
	}

	struct drm_mode_create_blob mb = { .data = U64(&mode), .length = sizeof mode };
	xioctl(DRM_IOCTL_MODE_CREATEPROPBLOB, &mb, "CREATEPROPBLOB", 1);

	add(conn_id, prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0), crtc_id, "CRTC_ID");
	add(crtc_id, prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", 0), mb.blob_id, "MODE_ID");
	add(crtc_id, prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 0), 1, "ACTIVE");
	uint32_t w = cd.width / halves, h = cd.height;
	for (int i = 0; i < halves; i++) {
		uint32_t p = planes[i], t = DRM_MODE_OBJECT_PLANE;
		add(p, prop_id(p, t, "FB_ID", 0), fb.fb_id, "FB_ID");
		add(p, prop_id(p, t, "CRTC_ID", 0), crtc_id, "CRTC_ID");
		add(p, prop_id(p, t, "SRC_X", 0), (uint64_t)(i * w) << 16, "SRC_X");
		add(p, prop_id(p, t, "SRC_Y", 0), 0, "SRC_Y");
		add(p, prop_id(p, t, "SRC_W", 0), (uint64_t)w << 16, "SRC_W");
		add(p, prop_id(p, t, "SRC_H", 0), (uint64_t)h << 16, "SRC_H");
		add(p, prop_id(p, t, "CRTC_X", 0), i * w, "CRTC_X");
		add(p, prop_id(p, t, "CRTC_Y", 0), 0, "CRTC_Y");
		add(p, prop_id(p, t, "CRTC_W", 0), w, "CRTC_W");
		add(p, prop_id(p, t, "CRTC_H", 0), h, "CRTC_H");
		uint32_t z = prop_id(p, t, "zpos", 0);
		if (z) add(p, z, 0, "zpos");   // same stage: SDE source split
	}

	// Pack requests per object for the atomic ioctl.
	uint32_t objs[16], counts[16], props[64]; uint64_t vals[64]; int no = 0, k = 0;
	for (int i = 0; i < nreq; i++) {
		int o; for (o = 0; o < no && objs[o] != reqs[i].obj; o++);
		if (o == no) { objs[no] = reqs[i].obj; counts[no++] = 0; }
	}
	for (int o = 0; o < no; o++)
		for (int i = 0; i < nreq; i++)
			if (reqs[i].obj == objs[o]) { props[k] = reqs[i].prop; vals[k++] = reqs[i].val; counts[o]++; }
	struct drm_mode_atomic at = { .flags = DRM_MODE_ATOMIC_ALLOW_MODESET, .count_objs = no,
				      .objs_ptr = U64(objs), .count_props_ptr = U64(counts),
				      .props_ptr = U64(props), .prop_values_ptr = U64(vals) };
	xioctl(DRM_IOCTL_MODE_ATOMIC, &at, "ATOMIC commit", 1);
	printf("atomic commit OK with %d plane(s) %u and %u, showing for %d s\n",
	       halves, planes[0], halves > 1 ? planes[1] : 0, seconds);
	fflush(stdout);
	sleep(seconds);
	return 0;
}
