// yupei-led: the RGB status LED next to the camera (PMIC LPG red/green/blue).
//
//   boot     white breathing, from the initramfs until "boot done" (SIGUSR1,
//            sent by yupei-led-boot.service once GDM is up): then white
//            fades to green, holds, and fades out. SIGUSR2 (emergency mode)
//            or no "done" within BOOT_TIMEOUT_S: red breathing for a while.
//   charging breathing whose colour follows the battery: red at <= 10 %
//            (fast, like a blink), through orange and yellow, to green at
//            100 % (slow); steady dim green when full. Off on battery.
//
// The LED has no hardware breathing (no pattern trigger), so this animates
// it from userspace at FPS frames per second while it is lit, and otherwise
// sleeps on power_supply uevents. Only one instance runs (abstract socket
// lock): the initramfs starts it, yupei-led.service starts it when that one
// did not.
//
// Build: gcc -O2 -static -o yupei-led yupei-led.c -lm
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LEDS "/sys/class/leds/"
#define BAT "/sys/class/power_supply/battery/"
#define FPS 30
#define PEAK 200.0          // of 255: bright enough, not blinding
#define BOOT_TIMEOUT_S 240

enum state { BOOT, BOOT_OK, BOOT_FAIL, IDLE };

static int fd[3] = { -1, -1, -1 };
static int last[3] = { -1, -1, -1 };
static volatile sig_atomic_t got_ok, got_fail, quit;

static void on_sig(int s)
{
	if (s == SIGUSR1) got_ok = 1;
	else if (s == SIGUSR2) got_fail = 1;
	else quit = 1;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int leds_open(void)
{
	static const char *names[3] = { "red", "green", "blue" };
	for (int i = 0; i < 3; i++) {
		if (fd[i] >= 0)
			continue;
		char p[64];
		snprintf(p, sizeof p, LEDS "%s/brightness", names[i]);
		fd[i] = open(p, O_WRONLY | O_CLOEXEC);
		if (fd[i] < 0)
			return -1;
	}
	return 0;
}

// r, g, b in 0..1 (linear light); the LPG PWM is linear too.
static void led(double r, double g, double b)
{
	double c[3] = { r, g, b };
	if (leds_open() < 0)
		return;
	for (int i = 0; i < 3; i++) {
		int v = (int)(PEAK * fmin(fmax(c[i], 0), 1) + 0.5);
		if (v == last[i])
			continue;
		char s[8];
		int n = snprintf(s, sizeof s, "%d", v);
		if (pwrite(fd[i], s, n, 0) == n)
			last[i] = v;
	}
}

// Smooth breathing level 0..1 for time t and period p: raised cosine,
// squared so the dark part lingers like a phone's notification LED.
static double breathe(double t, double p)
{
	double x = 0.5 - 0.5 * cos(2 * M_PI * t / p);
	return x * x;
}

static int read_int(const char *path, int def)
{
	char s[32];
	int f = open(path, O_RDONLY | O_CLOEXEC), n;
	if (f < 0)
		return def;
	n = read(f, s, sizeof s - 1);
	close(f);
	if (n <= 0)
		return def;
	s[n] = 0;
	return atoi(s);
}

// 0 = on battery, 1 = charging, 2 = full (and plugged in)
static int charge_state(int *capacity)
{
	char s[32] = "";
	int f = open(BAT "status", O_RDONLY | O_CLOEXEC);
	if (f >= 0) {
		int n = read(f, s, sizeof s - 1);
		s[n > 0 ? n : 0] = 0;
		close(f);
	}
	*capacity = read_int(BAT "capacity", 50);
	if (!strncmp(s, "Charging", 8))
		return *capacity >= 100 ? 2 : 1;
	if (!strncmp(s, "Full", 4))
		return 2;
	return 0;
}

// Battery colour: red (<= 10 %) -> orange -> yellow -> green (100 %),
// interpolated in hue so the middle stays bright.
static void battery_colour(int cap, double *r, double *g, double *b)
{
	double x = (fmin(fmax(cap, 10), 100) - 10) / 90.0;   // 0..1
	double hue = x * 120;                                   // degrees
	if (hue < 60) { *r = 1; *g = hue / 60; }
	else { *r = (120 - hue) / 60; *g = 1; }
	*g *= 0.75;   // the green die is much brighter than the red one
	*b = 0;
}

static int uevent_socket(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	int s = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
		       NETLINK_KOBJECT_UEVENT);
	if (s >= 0 && bind(s, (void *)&sa, sizeof sa) < 0) {
		close(s);
		s = -1;
	}
	return s;
}

static int single_instance(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char name[] = "yupei-led";
	memcpy(sa.sun_path + 1, name, sizeof name - 1);   // abstract namespace
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	return bind(s, (void *)&sa, sizeof(sa_family_t) + sizeof name) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
	enum state st = argc > 1 && !strcmp(argv[1], "--no-boot") ? IDLE : BOOT;
	if (single_instance() < 0)
		return 0;   // already running (started by the initramfs)
	struct sigaction sa = { .sa_handler = on_sig };
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	double t0 = now(), ts = t0;   // ts: when the current state began
	int ue = -1, cap = 50, chg = 0;
	double last_bat = -1;
	while (!quit) {
		double t = now();
		if (got_ok && st == BOOT) { st = BOOT_OK; ts = t; }
		if ((got_fail || t - t0 > BOOT_TIMEOUT_S) && st == BOOT) { st = BOOT_FAIL; ts = t; }
		got_ok = got_fail = 0;
		int wait_ms = 1000 / FPS;

		if (st == BOOT) {
			double v = breathe(t - t0, 2.4);
			led(v, v * 0.8, v * 0.7);   // warm-ish white on this RGB LED
		} else if (st == BOOT_OK) {
			// From wherever white is, 0.8 s to full green, hold 0.7 s,
			// 1.2 s fade out.
			double d = t - ts;
			static double w0 = -1;
			if (w0 < 0)
				w0 = last[0] < 0 ? 0 : last[0] / PEAK;
			if (d < 0.8) {
				double k = d / 0.8;
				led(w0 * (1 - k), w0 * 0.8 * (1 - k) + 0.8 * k, w0 * 0.7 * (1 - k));
			} else if (d < 1.5) {
				led(0, 0.8, 0);
			} else if (d < 2.7) {
				double k = 1 - (d - 1.5) / 1.2;
				led(0, 0.8 * k * k, 0);
			} else {
				led(0, 0, 0);
				st = IDLE;
				last_bat = -1;
			}
		} else if (st == BOOT_FAIL) {
			double d = t - ts;
			led(breathe(d, 1.2), 0, 0);
			if (d > 30) { st = IDLE; last_bat = -1; }
		} else {   // IDLE: charging indicator
			if (ue < 0)
				ue = uevent_socket();
			if (last_bat < 0 || t - last_bat > 20) {
				chg = charge_state(&cap);
				last_bat = t;
			}
			if (chg == 1) {
				double r, g, b, x = (fmin(fmax(cap, 10), 100) - 10) / 90.0;
				double period = 1.4 + 2.2 * x;   // 1.4 s at 10 % .. 3.6 s at 100 %
				battery_colour(cap, &r, &g, &b);
				double v = 0.04 + 0.96 * breathe(t - t0, period);
				led(r * v, g * v, b * v);
			} else if (chg == 2) {
				led(0, 0.15, 0);
				wait_ms = 20000;
			} else {
				led(0, 0, 0);
				wait_ms = 20000;
			}
			struct pollfd p = { .fd = ue, .events = POLLIN };
			if (ue >= 0 && poll(&p, 1, wait_ms) > 0) {
				char buf[4096];
				ssize_t n;
				while ((n = recv(ue, buf, sizeof buf - 1, 0)) > 0) {
					buf[n] = 0;
					if (strstr(buf, "power_supply"))
						last_bat = -1;   // re-read on the next frame
				}
			} else if (ue < 0) {
				usleep(wait_ms * 1000);
			}
			continue;
		}
		if (leds_open() < 0)
			wait_ms = 200;   // LED driver not loaded yet
		usleep(wait_ms * 1000);
	}
	led(0, 0, 0);
	return 0;
}
