// yupei-touchd: touch input for the Xiaomi Pad 8 without Xiaomi's THP HAL.
//
// The NT36532 touch controller runs in Xiaomi "THP" mode: it streams raw
// mutual-capacitance frames (60 x 40 cells, ~115 fps) and expects Android's
// proprietary libtouchreport to compute finger positions. This daemon reads
// the frames from /dev/xiaomi-touch (interface from Xiaomi's GPL touch
// driver), finds fingers itself and publishes them as a standard multi-touch
// screen through uinput ("yupei-thp-touch", 0..3199 x 0..2135, landscape).
//
// Frame: struct tp_frame { s64 time_ns; u64 frame_cnt; int fod_pressed;
// int fod_trackingId; char tp_raw[]; }; tp_raw[56] = type (3 = finger),
// [48]/[49] = cols/rows, int16 matrix at tp_raw + 64. A finger raises the
// cell values by ~1000 over a baseline of ~10000; noise is ~20.
// Measured orientation: column 0..59 = screen left..right, row 39..0 = top..bottom.
//
// Build: gcc -O2 -static -o yupei-touchd yupei-touchd.c -lm
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
	uint16_t x_resolution, y_resolution, rx_num, tx_num;
	uint8_t super_resolution_factor, frame_data_page_size, frame_data_buf_size;
	uint8_t raw_data_page_size, raw_data_buf_size;
	uint8_t lockdown_info[8];
	char config_file_name[64], driver_version[64], fw_version[64];
	uint8_t temp_change_value;
} hardware_param_t;

enum { COMMON_DATA_CMD, HARDWARE_PARAM_CMD, SELECT_MMAP_CMD, SELECT_TOUCH_ID,
       GET_FRAME_DATA_INDEX, RAW_DATA_INDEX, UPDATE_REPORT_POINT };
struct tp_frame_hdr { int64_t time_ns; uint64_t frame_cnt; int32_t fod_pressed, fod_tracking; };

#define SCREEN_W 3200
#define SCREEN_H 2136
#define MAXCELLS (64 * 64)
#define MAXF 10

// Tunables (signal units = raw counts above baseline).
static int T_PEAK = 300;    // a cell this far above baseline starts a finger
static int T_KEEP = 180;    // an existing finger survives down to this
static int T_STUCK = 150;   // baseline below raw by this much: baseline was taken
			    // with a finger on it, reset it
static double MATCH_PX = 450, SMOOTH = 0.45;
// Ghost touches: a charger shifts the panel's readings, and the slow baseline
// tracking below never follows a jump that big, so one spot reads as a
// finger forever (it toggled GNOME's calendar under the clock). Re-baseline
// when the charger state changes, and absorb any cell that stays raised.
#define STUCK_FRAMES (8 * 120)   // ~8 s at the panel's ~120 Hz frame rate
#define CHARGER_ONLINE "/sys/class/power_supply/usb/online"
static int GRACE = 2;       // frames a finger may go unseen before it is lifted:
			    // a fast swipe weakens the signal for a frame or two,
			    // and lifting there splits one swipe into many

static int ufd = -1;
static void emit(int type, int code, int val)
{
	struct input_event e = { .type = type, .code = code, .value = val };
	if (write(ufd, &e, sizeof e) < 0) { /* ignore */ }
}

static void uinput_setup(void)
{
	ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (ufd < 0) { perror("/dev/uinput"); exit(1); }
	ioctl(ufd, UI_SET_EVBIT, EV_KEY);
	ioctl(ufd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(ufd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
	ioctl(ufd, UI_SET_EVBIT, EV_ABS);
	ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	struct uinput_abs_setup a[] = {
		{ ABS_X, { .maximum = SCREEN_W - 1, .resolution = 12 } },
		{ ABS_Y, { .maximum = SCREEN_H - 1, .resolution = 12 } },
		{ ABS_MT_SLOT, { .maximum = MAXF - 1 } },
		{ ABS_MT_TRACKING_ID, { .maximum = 65535 } },
		{ ABS_MT_POSITION_X, { .maximum = SCREEN_W - 1, .resolution = 12 } },
		{ ABS_MT_POSITION_Y, { .maximum = SCREEN_H - 1, .resolution = 12 } },
		{ ABS_MT_TOUCH_MAJOR, { .maximum = 255 } },
	};
	for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++) {
		ioctl(ufd, UI_SET_ABSBIT, a[i].code);
		ioctl(ufd, UI_ABS_SETUP, &a[i]);
	}
	struct uinput_setup us = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x2717, .product = 0x8820 } };
	strcpy(us.name, "yupei-thp-touch");
	if (ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
		perror("uinput create");
		exit(1);
	}
}

struct finger { int active, id, miss; double x, y; int strength; };
static struct finger fing[MAXF];
static int next_id = 1;

struct cand { double x, y; int peak, used; };

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	const char *e;
	if ((e = getenv("YUPEI_TOUCH_PEAK"))) T_PEAK = atoi(e);
	if ((e = getenv("YUPEI_TOUCH_KEEP"))) T_KEEP = atoi(e);

	int fd = open("/dev/xiaomi-touch", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("/dev/xiaomi-touch"); return 1; }
	if (ioctl(fd, _IO('T', SELECT_TOUCH_ID), 0) < 0) { perror("SELECT_TOUCH_ID"); return 1; }
	hardware_param_t hp = {0};
	if (ioctl(fd, _IOR('T', HARDWARE_PARAM_CMD, hardware_param_t), &hp) < 0) { perror("HARDWARE_PARAM"); return 1; }
	size_t fsz = (size_t)hp.frame_data_page_size * 4096, total = fsz * hp.frame_data_buf_size;
	ioctl(fd, _IO('T', SELECT_MMAP_CMD), 1);
	uint8_t *ring = mmap(0, total, PROT_READ, MAP_SHARED, fd, 0);
	if (!total || ring == MAP_FAILED) { perror("mmap frames"); return 1; }
	uinput_setup();
	fprintf(stderr, "yupei-touchd: %ux%u cells, frames %zu x %u, thresholds peak %d keep %d\n",
		hp.tx_num, hp.rx_num, fsz, hp.frame_data_buf_size, T_PEAK, T_KEEP);

	static float base[MAXCELLS];
	static int sig[MAXCELLS];
	static int raised[MAXCELLS];
	int have_base = 0, charger = -1, nframe = 0, rebase_pending = 0;

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLPRI };
		if (poll(&p, 1, 1000) < 0 && errno != EINTR)
			break;
		int idx, latest = -1;
		while ((idx = ioctl(fd, _IO('T', GET_FRAME_DATA_INDEX), 0)) >= 0)
			latest = idx;   // only the newest frame matters
		if (latest < 0)
			continue;
		uint8_t *raw = ring + (size_t)latest * fsz + sizeof(struct tp_frame_hdr);
		if (raw[56] != 3)
			continue;
		int C = raw[48], R = raw[49], N = C * R;
		if (C < 3 || R < 3 || N > MAXCELLS)
			continue;
		int16_t *m = (int16_t *)(raw + 64);

		if (++nframe % 120 == 0) {   // ~1 s: charger plugged/unplugged?
			char c = 0;
			int f = open(CHARGER_ONLINE, O_RDONLY | O_CLOEXEC);
			if (f >= 0) {
				if (read(f, &c, 1) == 1 && charger >= 0 && (c == '1') != charger)
					rebase_pending = 1;
				charger = c == '1';
				close(f);
			}
		}
		if (rebase_pending) {   // wait until nothing touches the screen
			int any = 0;
			for (int f = 0; f < MAXF; f++)
				any |= fing[f].active;
			if (!any) {
				have_base = 0;
				rebase_pending = 0;
				fprintf(stderr, "yupei-touchd: charger %s, re-baselining\n",
					charger ? "connected" : "removed");
			}
		}
		if (!have_base) {
			for (int i = 0; i < N; i++) { base[i] = m[i]; raised[i] = 0; }
			have_base = 1;
			continue;
		}
		// Signal and baseline maintenance.
		int maxsig = 0;
		for (int i = 0; i < N; i++) {
			int s = m[i] - (int)base[i];
			if (s < -T_STUCK) {        // baseline captured under a finger
				base[i] = m[i];
				s = 0;
			} else if (s < T_KEEP / 2) {
				base[i] += (m[i] - base[i]) * 0.02f;   // slow drift tracking
			}
			if (s >= T_KEEP / 2) {
				if (++raised[i] > STUCK_FRAMES) {   // nobody presses one spot that long
					base[i] = m[i];
					s = 0;
					raised[i] = 0;
				}
			} else {
				raised[i] = 0;
			}
			sig[i] = s;
			if (s > maxsig) maxsig = s;
		}

		// Candidate fingers: local maxima, centroid over 3x3.
		struct cand cand[32];
		int nc = 0;
		if (maxsig >= T_KEEP) {
			for (int r = 0; r < R && nc < 32; r++)
				for (int c = 0; c < C && nc < 32; c++) {
					int s = sig[r * C + c];
					if (s < T_KEEP)
						continue;
					int is_max = 1;
					for (int dr = -1; dr <= 1 && is_max; dr++)
						for (int dc = -1; dc <= 1; dc++) {
							int rr = r + dr, cc = c + dc;
							if ((dr || dc) && rr >= 0 && rr < R && cc >= 0 && cc < C) {
								int t = sig[rr * C + cc];
								// strict on one side to break plateaus
								if (t > s || (t == s && (dr < 0 || (dr == 0 && dc < 0)))) {
									is_max = 0;
									break;
								}
							}
						}
					if (!is_max)
						continue;
					double wx = 0, wy = 0, w = 0;
					for (int dr = -1; dr <= 1; dr++)
						for (int dc = -1; dc <= 1; dc++) {
							int rr = r + dr, cc = c + dc;
							if (rr < 0 || rr >= R || cc < 0 || cc >= C)
								continue;
							double v = sig[rr * C + cc];
							if (v <= 0)
								continue;
							wx += v * cc; wy += v * rr; w += v;
						}
					double cx = wx / w, cy = wy / w;
					// col 0..C-1 -> left..right, row R-1..0 -> top..bottom
					cand[nc].x = cx / (C - 1) * (SCREEN_W - 1);
					cand[nc].y = (R - 1 - cy) / (R - 1) * (SCREEN_H - 1);
					cand[nc].peak = s;
					cand[nc].used = 0;
					nc++;
				}
		}

		// Track: keep existing fingers matched to the nearest candidate.
		int changed = 0;
		for (int f = 0; f < MAXF; f++) {
			if (!fing[f].active)
				continue;
			int best = -1;
			double bd = MATCH_PX;
			for (int k = 0; k < nc; k++) {
				if (cand[k].used || cand[k].peak < T_KEEP)
					continue;
				double d = hypot(cand[k].x - fing[f].x, cand[k].y - fing[f].y);
				if (d < bd) { bd = d; best = k; }
			}
			if (best < 0 && ++fing[f].miss <= GRACE)
				continue;   // maybe just a weak frame: hold position
			if (best < 0) {   // lifted
				fing[f].active = 0;
				emit(EV_ABS, ABS_MT_SLOT, f);
				emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
				changed = 1;
				continue;
			}
			cand[best].used = 1;
			fing[f].miss = 0;
			fing[f].x += (cand[best].x - fing[f].x) * (1 - SMOOTH);
			fing[f].y += (cand[best].y - fing[f].y) * (1 - SMOOTH);
			fing[f].strength = cand[best].peak;
			emit(EV_ABS, ABS_MT_SLOT, f);
			emit(EV_ABS, ABS_MT_POSITION_X, (int)lround(fing[f].x));
			emit(EV_ABS, ABS_MT_POSITION_Y, (int)lround(fing[f].y));
			changed = 1;
		}
		// New fingers need the stronger threshold.
		for (int k = 0; k < nc; k++) {
			if (cand[k].used || cand[k].peak < T_PEAK)
				continue;
			int near_held = 0;   // the weak finger in its grace frames came back
			for (int g = 0; g < MAXF; g++)
				if (fing[g].active && fing[g].miss &&
				    hypot(cand[k].x - fing[g].x, cand[k].y - fing[g].y) < MATCH_PX)
					near_held = 1;
			if (near_held)
				continue;
			int f;
			for (f = 0; f < MAXF && fing[f].active; f++);
			if (f == MAXF)
				break;
			fing[f] = (struct finger){ 1, next_id++ & 0xffff, 0, cand[k].x, cand[k].y, cand[k].peak };
			emit(EV_ABS, ABS_MT_SLOT, f);
			emit(EV_ABS, ABS_MT_TRACKING_ID, fing[f].id);
			emit(EV_ABS, ABS_MT_POSITION_X, (int)lround(fing[f].x));
			emit(EV_ABS, ABS_MT_POSITION_Y, (int)lround(fing[f].y));
			emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 30);
			changed = 1;
		}
		if (!changed)
			continue;
		int any = 0, first = -1;
		for (int f = 0; f < MAXF; f++)
			if (fing[f].active) { any++; if (first < 0) first = f; }
		emit(EV_KEY, BTN_TOUCH, any > 0);
		emit(EV_KEY, BTN_TOOL_FINGER, any > 0);
		if (first >= 0) {   // single-touch axes follow the first finger
			emit(EV_ABS, ABS_X, (int)lround(fing[first].x));
			emit(EV_ABS, ABS_Y, (int)lround(fing[first].y));
		}
		emit(EV_SYN, SYN_REPORT, 0);
	}
	return 0;
}
