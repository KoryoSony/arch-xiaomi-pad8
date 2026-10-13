// thp-capture: inspect Xiaomi THP touch frames on yupei (debug helper).
// Reads frames from /dev/xiaomi-touch for N seconds and prints statistics and
// an ASCII heat map of the strongest frame. Frame layout (from Xiaomi's
// nt36532 driver, nvt_ts_prase_data_func): struct tp_frame { s64 time_ns;
// u64 frame_cnt; int fod_pressed; int fod_trackingId; char tp_raw[5192]; }
// where tp_raw[56] is the data type (3 = finger), [48]/[49] numCol/numRow,
// and the int16 matrix starts at tp_raw + 64.
// Usage: thp-capture [seconds]
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
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

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 15;
	int fd = open("/dev/xiaomi-touch", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("/dev/xiaomi-touch"); return 1; }
	if (ioctl(fd, _IO('T', SELECT_TOUCH_ID), 0) < 0) perror("SELECT_TOUCH_ID");
	hardware_param_t hp = {0};
	if (ioctl(fd, _IOR('T', HARDWARE_PARAM_CMD, hardware_param_t), &hp) < 0) perror("HARDWARE_PARAM");
	printf("hw: res %ux%u rx %u tx %u srf %u frame pages %u bufs %u raw pages %u bufs %u cfg '%s' fw '%s'\n",
	       hp.x_resolution, hp.y_resolution, hp.rx_num, hp.tx_num, hp.super_resolution_factor,
	       hp.frame_data_page_size, hp.frame_data_buf_size, hp.raw_data_page_size, hp.raw_data_buf_size,
	       hp.config_file_name, hp.fw_version);
	size_t fsz = (size_t)hp.frame_data_page_size * 4096, total = fsz * hp.frame_data_buf_size;
	if (!total) { fprintf(stderr, "no frame buffer\n"); return 1; }
	ioctl(fd, _IO('T', SELECT_MMAP_CMD), 1);
	uint8_t *ring = mmap(0, total, PROT_READ, MAP_SHARED, fd, 0);
	if (ring == MAP_FAILED) { perror("mmap frames"); return 1; }

	// Record every finger frame, then use the per-cell median over the whole
	// capture as baseline (a finger covers any one cell only briefly), and
	// report per second where the strongest dip and rise were.
	int frames = 0, cols = 0, rows = 0, nrec = 0, maxrec = secs * 130;
	int16_t *rec = malloc((size_t)maxrec * 256 * 64 * 2);
	int *rec_sec = malloc(sizeof(int) * maxrec);
	time_t start = time(0), end = start + secs;
	while (time(0) < end && nrec < maxrec) {
		struct pollfd p = { .fd = fd, .events = POLLPRI };
		poll(&p, 1, 200);
		int idx;
		while (nrec < maxrec && (idx = ioctl(fd, _IO('T', GET_FRAME_DATA_INDEX), 0)) >= 0) {
			uint8_t *raw = ring + (size_t)idx * fsz + sizeof(struct tp_frame_hdr);
			frames++;
			if (raw[56] != 3)
				continue;
			int c = raw[48], r = raw[49];
			if (!c || !r || c * r > 256 * 64)
				continue;
			cols = c; rows = r;
			memcpy(rec + (size_t)nrec * c * r, raw + 64, (size_t)c * r * 2);
			rec_sec[nrec++] = (int)(time(0) - start);
		}
	}
	int n = cols * rows;
	printf("frames %d, recorded %d finger frames of %d cols x %d rows\n", frames, nrec, cols, rows);
	if (!nrec)
		return 1;
	int16_t *col = malloc(sizeof(int16_t) * nrec);
	int32_t *base = malloc(sizeof(int32_t) * n);
	for (int i = 0; i < n; i++) {   // median via counting sort window
		for (int f = 0; f < nrec; f++) col[f] = rec[(size_t)f * n + i];
		// insertion-free median: nth_element by partial selection
		int k = nrec / 2;
		int lo = 0, hi = nrec - 1;
		while (lo < hi) {
			int16_t pv = col[(lo + hi) / 2];
			int a = lo, b = hi;
			while (a <= b) {
				while (col[a] < pv) a++;
				while (col[b] > pv) b--;
				if (a <= b) { int16_t t = col[a]; col[a] = col[b]; col[b] = t; a++; b--; }
			}
			if (k <= b) hi = b; else if (k >= a) lo = a; else break;
		}
		base[i] = col[k];
	}
	for (int sec = 0; sec <= secs; sec++) {
		int dip = 0, dx = 0, dy = 0, rise = 0, rx = 0, ry = 0, cnt = 0;
		for (int f = 0; f < nrec; f++) {
			if (rec_sec[f] != sec)
				continue;
			cnt++;
			for (int i = 0; i < n; i++) {
				int d = base[i] - rec[(size_t)f * n + i];
				if (d > dip) { dip = d; dx = i % cols; dy = i / cols; }
				if (-d > rise) { rise = -d; rx = i % cols; ry = i / cols; }
			}
		}
		if (cnt)
			printf("t=%2ds  dip %5d at col %2d row %2d   rise %5d at col %2d row %2d\n",
			       sec, dip, dx, dy, rise, rx, ry);
	}
	return 0;
}
