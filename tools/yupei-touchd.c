// yupei-touchd: touch input for the Xiaomi Pad 8 without Xiaomi's THP HAL.
//
// The NT36532 touch controller runs in Xiaomi "THP" mode: it streams raw
// mutual-capacitance frames (60 x 40 cells, 120 Hz) and expects Android's
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
// Pipeline, every frame (none are skipped: a quick tap lasts only a few):
//  1. signal = raw - baseline; the baseline follows slow drift, is re-taken
//     when the charger is plugged/unplugged (it shifts the readings) and
//     absorbs cells that stay raised for seconds (ghost touches).
//  2. contacts = local maxima, merged when two maxima belong to one blob;
//     position by Gaussian sub-cell interpolation of the peak and its
//     neighbours (no inward bias at the screen edges, unlike a clipped
//     centroid: the keyboard's bottom row is on the edge).
//  3. tracking: each finger is matched to the nearest contact around where
//     it is predicted to be (position + velocity); the search radius grows
//     with speed. A contact that is not near any finger is a new touch, so
//     two quick taps on different keys stay two taps.
//  4. a finger goes down at the first frame above T_DOWN and up as soon as
//     its contact falls below T_UP (hysteresis); fast-moving fingers may
//     skip one weak frame.
//  5. 1-euro filter: steady when still, no lag when moving.
//
// Testing without the screen:
//   YUPEI_TOUCH_RECORD=file  also append every frame to file
//   yupei-touchd --replay file   run a recording through the decoder and
//                                print the touch events instead of uinput
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
#define MAXC 32

// Signal thresholds (raw counts above baseline; a finger peaks at ~600-1500).
static int T_DOWN = 260;   // a new touch
static int T_UP = 140;     // a touching finger lifts below this
#define T_STUCK 150        // baseline below raw by this much: taken under a finger
#define STUCK_FRAMES (8 * 120)   // a cell raised ~8 s: ghost, absorb it
#define CHARGER_ONLINE "/sys/class/power_supply/usb/online"

// Tracking.
#define MATCH_BASE_PX 160.0    // search radius around the predicted position
#define MATCH_SPEED_K 1.6      // + this many frames of travel at current speed
#define MATCH_MAX_PX 420.0
#define FAST_PX_S 1500.0       // above this a finger may skip one weak frame

// 1-euro filter (Casiez et al.), per axis, in pixels.
#define EURO_MINCUT 4.0        // Hz, at rest (measured: 0.5 px jitter, 2 ms lag at 1500 px/s)
#define EURO_BETA 0.05         // cutoff increase per px/s
#define EURO_DCUT 1.0

#define CELL_W ((double)(SCREEN_W - 1))
#define CELL_H ((double)(SCREEN_H - 1))

struct euro { double x, dx; int init; };

static double euro_alpha(double cutoff, double dt)
{
	double tau = 1.0 / (2 * M_PI * cutoff);
	return 1.0 / (1.0 + tau / dt);
}

static double euro_filter(struct euro *e, double v, double dt)
{
	if (!e->init || dt <= 0) {
		e->x = v;
		e->dx = 0;
		e->init = 1;
		return v;
	}
	double dv = (v - e->x) / dt;
	double ad = euro_alpha(EURO_DCUT, dt);
	e->dx += ad * (dv - e->dx);
	double a = euro_alpha(EURO_MINCUT + EURO_BETA * fabs(e->dx), dt);
	e->x += a * (v - e->x);
	return e->x;
}

struct finger {
	int active, id, missed;
	double x, y;        // raw tracked position (px)
	double vx, vy;      // px per second
	double ox, oy;      // last reported (filtered) position
	struct euro ex, ey;
	int major;
	int64_t t;          // time of last update (ns)
};

struct cand { double x, y; int peak, major, used; };

static struct finger fing[MAXF];
static int next_id = 1;

// ---------------------------------------------------------------- output
static int ufd = -1;
static int replay;
static int64_t replay_t0;

static void emit(int type, int code, int val)
{
	struct input_event e = { .type = type, .code = code, .value = val };
	if (ufd >= 0 && write(ufd, &e, sizeof e) < 0) { /* ignore */ }
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

static void report(int64_t t, const char *what, int f)
{
	if (replay)
		printf("%9.3f %-4s id %-4d x %4.0f y %4.0f%s\n", (t - replay_t0) / 1e9, what,
		       fing[f].id, fing[f].ox, fing[f].oy, "");
}

// ---------------------------------------------------------------- decoder
static float base[MAXCELLS];
static int sig[MAXCELLS], raised[MAXCELLS];
static int have_base, rebase_pending;

static inline int S(int c, int r, int C, int R)
{
	if (c < 0 || c >= C || r < 0 || r >= R)
		return 0;
	return sig[r * C + c];
}

// Sub-cell offset of a peak (in cells, -0.5..0.5) from its two neighbours
// along one axis. Gaussian interpolation (log-parabola) suits the blob shape;
// at the screen edge the outer neighbour is missing: the finger centre then
// lies between the edge cell and its inner neighbour, estimated from their
// ratio.
static double subcell(double l, double c, double r, int at_low_edge, int at_high_edge)
{
	if (c <= 1)
		return 0;
	if (at_low_edge)
		return 0.5 * fmax(0, r) / c;
	if (at_high_edge)
		return -0.5 * fmax(0, l) / c;
	l = fmax(l, 1); r = fmax(r, 1);
	double ll = log(l), lc = log(c), lr = log(r);
	double den = ll - 2 * lc + lr;
	if (den >= -1e-6)
		return 0;
	double d = 0.5 * (ll - lr) / den;
	return d < -0.5 ? -0.5 : d > 0.5 ? 0.5 : d;
}

static int find_candidates(int C, int R, struct cand *cand)
{
	int nc = 0;
	struct { int c, r, s; } pk[64];
	int np = 0;
	for (int r = 0; r < R; r++)
		for (int c = 0; c < C; c++) {
			int s = sig[r * C + c];
			if (s < T_UP)
				continue;
			int is_max = 1;
			for (int dr = -1; dr <= 1 && is_max; dr++)
				for (int dc = -1; dc <= 1; dc++) {
					if (!dr && !dc)
						continue;
					int t = S(c + dc, r + dr, C, R);
					// strict on one side to break plateaus
					if (t > s || (t == s && (dr < 0 || (dr == 0 && dc < 0)))) {
						is_max = 0;
						break;
					}
				}
			if (is_max && np < 64)
				pk[np++] = (typeof(pk[0])){ c, r, s };
		}
	// strongest first; a weaker maximum belongs to the same blob when the
	// signal between them never dips well below it (one finger, two bumps)
	for (int i = 0; i < np; i++)
		for (int j = i + 1; j < np; j++)
			if (pk[j].s > pk[i].s) { typeof(pk[0]) t = pk[i]; pk[i] = pk[j]; pk[j] = t; }
	for (int i = 0; i < np && nc < MAXC; i++) {
		int merged = 0;
		for (int k = 0; k < nc && !merged; k++) {
			int kc = (int)lround(cand[k].x / CELL_W * (C - 1));
			int kr = (int)lround((R - 1) - cand[k].y / CELL_H * (R - 1));
			int dc = pk[i].c - kc, dr = pk[i].r - kr;
			if (dc * dc + dr * dr > 9)
				continue;
			int valley = 1 << 30;
			for (int st = 1; st < 8; st++) {
				int c = kc + (int)lround(dc * st / 8.0), r = kr + (int)lround(dr * st / 8.0);
				int v = S(c, r, C, R);
				if (v < valley) valley = v;
			}
			if (valley > pk[i].s * 0.6)
				merged = 1;
		}
		if (merged)
			continue;
		int c = pk[i].c, r = pk[i].r, s = pk[i].s;
		double ox = subcell(S(c - 1, r, C, R), s, S(c + 1, r, C, R), c == 0, c == C - 1);
		double oy = subcell(S(c, r - 1, C, R), s, S(c, r + 1, C, R), r == 0, r == R - 1);
		double cx = c + ox, cy = r + oy;
		// contact size: cells above half the peak in a 5x5 window
		int area = 0;
		for (int dr = -2; dr <= 2; dr++)
			for (int dc = -2; dc <= 2; dc++)
				if (S(c + dc, r + dr, C, R) * 2 >= s)
					area++;
		cand[nc].x = cx / (C - 1) * CELL_W;
		cand[nc].y = (R - 1 - cy) / (R - 1) * CELL_H;
		cand[nc].peak = s;
		cand[nc].major = area * 12 > 255 ? 255 : area * 12;
		cand[nc].used = 0;
		nc++;
	}
	return nc;
}

static void lift(int f, int64_t t)
{
	fing[f].active = 0;
	emit(EV_ABS, ABS_MT_SLOT, f);
	emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
	report(t, "up", f);
}

static void process_frame(const int16_t *m, int C, int R, int64_t t)
{
	int N = C * R;
	if (rebase_pending) {   // wait until nothing touches the screen
		int any = 0;
		for (int f = 0; f < MAXF; f++)
			any |= fing[f].active;
		if (!any) {
			have_base = 0;
			rebase_pending = 0;
		}
	}
	if (!have_base) {
		for (int i = 0; i < N; i++) { base[i] = m[i]; raised[i] = 0; }
		have_base = 1;
		return;
	}
	for (int i = 0; i < N; i++) {
		int s = m[i] - (int)base[i];
		if (s < -T_STUCK) {             // baseline captured under a finger
			base[i] = m[i];
			s = 0;
		} else if (s < T_UP / 2) {
			base[i] += (m[i] - base[i]) * 0.02f;   // slow drift tracking
		}
		if (s >= T_UP / 2) {
			if (++raised[i] > STUCK_FRAMES) {      // nobody presses one spot that long
				base[i] = m[i];
				s = 0;
				raised[i] = 0;
			}
		} else {
			raised[i] = 0;
		}
		sig[i] = s;
	}

	struct cand cand[MAXC];
	int nc = find_candidates(C, R, cand);

	// Match fingers to contacts: all pairs within each finger's radius
	// around its predicted position, closest first.
	struct { int f, k; double d; } pair[MAXF * MAXC];
	int npair = 0;
	for (int f = 0; f < MAXF; f++) {
		if (!fing[f].active)
			continue;
		double dt = (t - fing[f].t) / 1e9;
		if (dt <= 0 || dt > 0.1) dt = 1 / 120.0;
		double px = fing[f].x + fing[f].vx * dt, py = fing[f].y + fing[f].vy * dt;
		double speed = hypot(fing[f].vx, fing[f].vy);
		double rad = fmin(MATCH_MAX_PX, MATCH_BASE_PX + MATCH_SPEED_K * speed / 120.0);
		for (int k = 0; k < nc; k++) {
			double d = hypot(cand[k].x - px, cand[k].y - py);
			if (d < rad)
				pair[npair++] = (typeof(pair[0])){ f, k, d };
		}
	}
	for (int i = 0; i < npair; i++)
		for (int j = i + 1; j < npair; j++)
			if (pair[j].d < pair[i].d) { typeof(pair[0]) tmp = pair[i]; pair[i] = pair[j]; pair[j] = tmp; }
	int matched[MAXF] = { 0 };
	int changed = 0;
	for (int i = 0; i < npair; i++) {
		int f = pair[i].f, k = pair[i].k;
		if (matched[f] || cand[k].used)
			continue;
		matched[f] = 1;
		cand[k].used = 1;
		double dt = (t - fing[f].t) / 1e9;
		if (dt <= 0 || dt > 0.1) dt = 1 / 120.0;
		double nvx = (cand[k].x - fing[f].x) / dt, nvy = (cand[k].y - fing[f].y) / dt;
		fing[f].vx += 0.5 * (nvx - fing[f].vx);
		fing[f].vy += 0.5 * (nvy - fing[f].vy);
		fing[f].x = cand[k].x;
		fing[f].y = cand[k].y;
		fing[f].t = t;
		fing[f].missed = 0;
		fing[f].major = cand[k].major;
		fing[f].ox = euro_filter(&fing[f].ex, fing[f].x, dt);
		fing[f].oy = euro_filter(&fing[f].ey, fing[f].y, dt);
		emit(EV_ABS, ABS_MT_SLOT, f);
		emit(EV_ABS, ABS_MT_POSITION_X, (int)lround(fing[f].ox));
		emit(EV_ABS, ABS_MT_POSITION_Y, (int)lround(fing[f].oy));
		emit(EV_ABS, ABS_MT_TOUCH_MAJOR, fing[f].major);
		if (replay && getenv("YUPEI_TOUCH_TRACE"))
			report(t, "move", f);
		changed = 1;
	}
	// Unmatched fingers lift, except a fast-moving one gets one frame of grace.
	for (int f = 0; f < MAXF; f++) {
		if (!fing[f].active || matched[f])
			continue;
		if (hypot(fing[f].vx, fing[f].vy) > FAST_PX_S && fing[f].missed++ < 1)
			continue;
		lift(f, t);
		changed = 1;
	}
	// New touches: strong contacts that no finger claimed.
	for (int k = 0; k < nc; k++) {
		if (cand[k].used || cand[k].peak < T_DOWN)
			continue;
		int f;
		for (f = 0; f < MAXF && fing[f].active; f++);
		if (f == MAXF)
			break;
		fing[f] = (struct finger){ .active = 1, .id = next_id++ & 0xffff,
					   .x = cand[k].x, .y = cand[k].y, .ox = cand[k].x,
					   .oy = cand[k].y, .major = cand[k].major, .t = t };
		euro_filter(&fing[f].ex, fing[f].x, 0);
		euro_filter(&fing[f].ey, fing[f].y, 0);
		emit(EV_ABS, ABS_MT_SLOT, f);
		emit(EV_ABS, ABS_MT_TRACKING_ID, fing[f].id);
		emit(EV_ABS, ABS_MT_POSITION_X, (int)lround(fing[f].x));
		emit(EV_ABS, ABS_MT_POSITION_Y, (int)lround(fing[f].y));
		emit(EV_ABS, ABS_MT_TOUCH_MAJOR, fing[f].major);
		report(t, "down", f);
		changed = 1;
	}
	if (!changed)
		return;
	int any = 0, first = -1;
	for (int f = 0; f < MAXF; f++)
		if (fing[f].active) { any++; if (first < 0) first = f; }
	emit(EV_KEY, BTN_TOUCH, any > 0);
	emit(EV_KEY, BTN_TOOL_FINGER, any > 0);
	if (first >= 0) {   // single-touch axes follow the first finger
		emit(EV_ABS, ABS_X, (int)lround(fing[first].ox));
		emit(EV_ABS, ABS_Y, (int)lround(fing[first].oy));
	}
	emit(EV_SYN, SYN_REPORT, 0);
}

// ---------------------------------------------------------------- replay
// Recording format, per frame: int64 time_ns, uint8 cols, uint8 rows,
// cols*rows int16 values.
static int run_replay(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	replay = 1;
	int64_t t;
	uint8_t cr[2];
	static int16_t m[MAXCELLS];
	long frames = 0;
	while (fread(&t, sizeof t, 1, fp) == 1 && fread(cr, 2, 1, fp) == 1) {
		int N = cr[0] * cr[1];
		if (N > MAXCELLS || fread(m, 2, N, fp) != (size_t)N)
			break;
		if (!frames)
			replay_t0 = t;
		frames++;
		process_frame(m, cr[0], cr[1], t);
	}
	fprintf(stderr, "replayed %ld frames\n", frames);
	return 0;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	const char *e;
	if ((e = getenv("YUPEI_TOUCH_DOWN"))) T_DOWN = atoi(e);
	if ((e = getenv("YUPEI_TOUCH_UP"))) T_UP = atoi(e);
	if (argc == 3 && !strcmp(argv[1], "--replay"))
		return run_replay(argv[2]);

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
	FILE *rec = NULL;
	if ((e = getenv("YUPEI_TOUCH_RECORD")) && *e)
		rec = fopen(e, "ab");
	fprintf(stderr, "yupei-touchd: %ux%u cells, frames %zu x %u, down %d up %d%s\n",
		hp.tx_num, hp.rx_num, fsz, hp.frame_data_buf_size, T_DOWN, T_UP,
		rec ? ", recording" : "");

	int charger = -1, nframe = 0;
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLPRI };
		if (poll(&p, 1, 1000) < 0 && errno != EINTR)
			break;
		int idx;
		// every queued frame, in order: a quick tap lasts only a few
		while ((idx = ioctl(fd, _IO('T', GET_FRAME_DATA_INDEX), 0)) >= 0) {
			const struct tp_frame_hdr *h = (const void *)(ring + (size_t)idx * fsz);
			const uint8_t *raw = (const uint8_t *)(h + 1);
			if (raw[56] != 3)
				continue;
			int C = raw[48], R = raw[49];
			if (C < 3 || R < 3 || C * R > MAXCELLS)
				continue;
			const int16_t *m = (const int16_t *)(raw + 64);
			if (++nframe % 120 == 0) {   // ~1 s: charger plugged/unplugged?
				char c = 0;
				int f = open(CHARGER_ONLINE, O_RDONLY | O_CLOEXEC);
				if (f >= 0) {
					if (read(f, &c, 1) == 1 && charger >= 0 && (c == '1') != charger) {
						rebase_pending = 1;
						fprintf(stderr, "yupei-touchd: charger %s, re-baselining\n",
							c == '1' ? "connected" : "removed");
					}
					charger = c == '1';
					close(f);
				}
			}
			if (rec) {
				uint8_t cr[2] = { C, R };
				fwrite(&h->time_ns, sizeof h->time_ns, 1, rec);
				fwrite(cr, 2, 1, rec);
				fwrite(m, 2, C * R, rec);
			}
			process_frame(m, C, R, h->time_ns);
		}
		if (rec)
			fflush(rec);
	}
	return 0;
}
