// kmstest: minimal KMS check for the yupei panel, no libdrm needed.
// Finds the first connected connector, sets its preferred mode on a free CRTC
// with a dumb framebuffer showing colour bars, then waits (default 20 s).
// Build: gcc -O2 -static -o kmstest kmstest.c
#include <drm/drm.h>
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

static int fd;

static void xioctl(unsigned long req, void *arg, const char *what)
{
	if (ioctl(fd, req, arg) < 0) {
		fprintf(stderr, "%s: %s\n", what, strerror(errno));
		exit(1);
	}
}

#define U64(p) ((uint64_t)(uintptr_t)(p))

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 20;
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open /dev/dri/card0"); return 1; }
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0)
		fprintf(stderr, "warning: SET_MASTER: %s\n", strerror(errno));

	struct drm_mode_card_res res = {0};
	xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	uint32_t *crtcs = calloc(res.count_crtcs, 4), *conns = calloc(res.count_connectors, 4);
	uint32_t *encs = calloc(res.count_encoders, 4), *fbs = calloc(res.count_fbs + 1, 4);
	res.crtc_id_ptr = U64(crtcs); res.connector_id_ptr = U64(conns);
	res.encoder_id_ptr = U64(encs); res.fb_id_ptr = U64(fbs);
	xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	printf("crtcs=%u connectors=%u encoders=%u max=%ux%u\n", res.count_crtcs,
	       res.count_connectors, res.count_encoders, res.max_width, res.max_height);

	struct drm_mode_get_connector conn;
	struct drm_mode_modeinfo mode;
	uint32_t conn_id = 0, enc_ids[16];
	int nenc = 0;
	// Prefer the built-in panel (DSI); SDE's writeback/virtual connectors also
	// claim to be connected. Pass 0 takes DSI only, pass 1 anything real.
	for (int pass = 0; pass < 2 && !conn_id; pass++)
	for (uint32_t i = 0; i < res.count_connectors && !conn_id; i++) {
		memset(&conn, 0, sizeof conn);
		conn.connector_id = conns[i];
		xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR");
		printf("connector %u type %u connection %u modes %u\n", conns[i],
		       conn.connector_type, conn.connection, conn.count_modes);
		if (conn.connection != 1 || !conn.count_modes)
			continue;
		if (conn.connector_type == DRM_MODE_CONNECTOR_VIRTUAL ||
		    conn.connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			continue;
		if (pass == 0 && conn.connector_type != DRM_MODE_CONNECTOR_DSI)
			continue;
		struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof *modes);
		uint32_t *props = calloc(conn.count_props + 1, 4);
		uint64_t *vals = calloc(conn.count_props + 1, 8);
		nenc = conn.count_encoders > 16 ? 16 : conn.count_encoders;
		conn.modes_ptr = U64(modes); conn.encoders_ptr = U64(enc_ids);
		conn.props_ptr = U64(props); conn.prop_values_ptr = U64(vals);
		conn.count_encoders = nenc;
		xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR");
		mode = modes[0];
		for (uint32_t m = 0; m < conn.count_modes; m++) {
			printf("  mode %u: %s %ux%u@%u clock=%u flags=0x%x type=0x%x\n", m, modes[m].name,
			       modes[m].hdisplay, modes[m].vdisplay, modes[m].vrefresh,
			       modes[m].clock, modes[m].flags, modes[m].type);
			if (modes[m].type & DRM_MODE_TYPE_PREFERRED) mode = modes[m];
		}
		if (argc > 2 && atoi(argv[2]) < (int)conn.count_modes)
			mode = modes[atoi(argv[2])];   // kmstest SECONDS MODE_INDEX
		conn_id = conns[i];
		printf("connector %u connected, %u modes, using %s %ux%u@%u\n", conn_id,
		       conn.count_modes, mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);
	}
	if (!conn_id) { fprintf(stderr, "no connected connector\n"); return 1; }

	uint32_t crtc_id = 0;
	for (int e = 0; e < nenc && !crtc_id; e++) {
		struct drm_mode_get_encoder enc = { .encoder_id = enc_ids[e] };
		xioctl(DRM_IOCTL_MODE_GETENCODER, &enc, "GETENCODER");
		if (enc.crtc_id) { crtc_id = enc.crtc_id; break; }
		for (uint32_t c = 0; c < res.count_crtcs; c++)
			if (enc.possible_crtcs & (1u << c)) { crtc_id = crtcs[c]; break; }
	}
	if (!crtc_id) { fprintf(stderr, "no usable crtc\n"); return 1; }
	printf("crtc %u\n", crtc_id);

	struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
	xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd, "CREATE_DUMB");
	struct drm_mode_fb_cmd fb = { .width = cd.width, .height = cd.height, .pitch = cd.pitch,
				      .bpp = 32, .depth = 24, .handle = cd.handle };
	xioctl(DRM_IOCTL_MODE_ADDFB, &fb, "ADDFB");
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	xioctl(DRM_IOCTL_MODE_MAP_DUMB, &md, "MAP_DUMB");
	uint8_t *px = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (px == MAP_FAILED) { perror("mmap"); return 1; }

	// Colour bars, with a big white "ARCH" banner made of blocks in the middle.
	static const uint32_t bars[] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
					  0xff00ff, 0xff0000, 0x0000ff, 0x1793d1 };
	static const char *banner[] = {
		" ### ####   ### #  #",
		"#  # #   # #    #  #",
		"#### ####  #    ####",
		"#  # #  #  #    #  #",
		"#  # #   #  ### #  #",
	};
	int bw = 20, bh = 5, cell = cd.width / 32;
	int bx = (cd.width - bw * cell) / 2, by = (cd.height - bh * cell) / 2;
	for (uint32_t y = 0; y < cd.height; y++) {
		uint32_t *row = (uint32_t *)(px + y * cd.pitch);
		for (uint32_t x = 0; x < cd.width; x++) {
			uint32_t c = bars[x * 8 / cd.width];
			int cx = ((int)x - bx) / cell, cy = ((int)y - by) / cell;
			if ((int)x >= bx && (int)y >= by && cx < bw && cy < bh)
				c = banner[cy][cx] == '#' ? 0xffffff : 0x000000;
			row[x] = c;
		}
	}

	struct drm_mode_crtc sc = { .crtc_id = crtc_id, .fb_id = fb.fb_id,
				    .set_connectors_ptr = U64(&conn_id), .count_connectors = 1,
				    .mode = mode, .mode_valid = 1 };
	xioctl(DRM_IOCTL_MODE_SETCRTC, &sc, "SETCRTC");
	printf("modeset OK, showing pattern for %d s\n", seconds);
	fflush(stdout);
	sleep(seconds);
	return 0;
}
