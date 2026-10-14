// yupei-powerkey: power button and magnetic cover handling for the GNOME
// session on yupei. logind is told to ignore both (/etc/systemd/logind.conf.d/
// yupei.conf); this runs inside the session (/etc/xdg/autostart).
//   Power tap:  $YUPEI_POWERKEY_SHORT "on"|"off" (default /usr/lib/yupei/
//               power-short), with the panel state from *before* the press:
//               mutter wakes the panel on any key, so asking it after the
//               press would always say "on" and blank it again.
//   Power hold (>= 0.7 s): GNOME's power-off dialog
//               (org.gnome.SessionManager.Shutdown).
//   Cover (xiaomi-hall SW_LID): $YUPEI_COVER close|open (default
//               /usr/lib/yupei/cover).
// Build: gcc -O2 -static -o yupei-powerkey yupei-powerkey.c
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define LONG_MS 700
#define PANEL_DPMS "/sys/class/drm/card0-DSI-1/dpms"

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void spawn(const char *cmd, const char *arg)
{
	fprintf(stderr, "yupei-powerkey: %s %s\n", cmd, arg ? arg : "");
	if (fork() == 0) {
		if (arg)
			execl("/bin/sh", "sh", "-c", "exec $0 \"$1\"", cmd, arg, (char *)NULL);
		else
			execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static const char *env_or(const char *name, const char *def)
{
	const char *v = getenv(name);
	return v && *v ? v : def;
}

static int panel_on(void)
{
	char buf[8] = "";
	int f = open(PANEL_DPMS, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return 1;
	int n = read(f, buf, sizeof buf - 1);
	close(f);
	return n <= 0 || strncmp(buf, "Off", 3) != 0;
}

static int open_input(const char *want)
{
	for (int i = 0; i < 64; i++) {
		char p[32], name[64] = "";
		snprintf(p, sizeof p, "/dev/input/event%d", i);
		int f = open(p, O_RDONLY | O_CLOEXEC);
		if (f < 0)
			continue;
		ioctl(f, EVIOCGNAME(sizeof name), name);
		if (!strcmp(name, want))
			return f;
		close(f);
	}
	return -1;
}

int main(void)
{
	struct pollfd p[2] = {
		{ .fd = open_input("pmic_pwrkey"), .events = POLLIN },
		{ .fd = open_input("xiaomi-hall"), .events = POLLIN },
	};
	if (p[0].fd < 0)
		fprintf(stderr, "yupei-powerkey: pmic_pwrkey not found\n");
	if (p[1].fd < 0)
		fprintf(stderr, "yupei-powerkey: xiaomi-hall not found\n");
	if (p[0].fd < 0 && p[1].fd < 0)
		return 1;
	const char *short_cmd = env_or("YUPEI_POWERKEY_SHORT", "/usr/lib/yupei/power-short");
	const char *cover_cmd = env_or("YUPEI_COVER", "/usr/lib/yupei/cover");
	signal(SIGCHLD, SIG_IGN);
	long down = -1;
	int long_fired = 0, was_on = 1;
	for (;;) {
		int timeout = -1;
		if (down >= 0 && !long_fired) {
			timeout = (int)(LONG_MS - (now_ms() - down));
			if (timeout < 0)
				timeout = 0;
		}
		int r = poll(p, 2, timeout);
		if (r == 0 && down >= 0 && !long_fired) {   // held long enough
			long_fired = 1;
			spawn("gdbus call --session --dest org.gnome.SessionManager "
			      "--object-path /org/gnome/SessionManager "
			      "--method org.gnome.SessionManager.Shutdown >/dev/null", NULL);
			continue;
		}
		for (int i = 0; i < 2; i++) {
			struct input_event e;
			if (!(p[i].revents & POLLIN) || read(p[i].fd, &e, sizeof e) != sizeof e)
				continue;
			if (i == 0 && e.type == EV_KEY && e.code == KEY_POWER) {
				if (e.value == 1) {
					was_on = panel_on();
					down = now_ms();
					long_fired = 0;
				} else if (e.value == 0 && down >= 0) {
					if (!long_fired)
						spawn(short_cmd, was_on ? "on" : "off");
					down = -1;
				}
			} else if (i == 1 && e.type == EV_SW && e.code == SW_LID) {
				spawn(cover_cmd, e.value ? "close" : "open");
			}
		}
	}
}
