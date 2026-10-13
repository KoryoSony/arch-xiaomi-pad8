// yupei-powerkey: power button handling for the GNOME session on yupei.
// Short press: toggle the screen (/usr/lib/yupei/screen-toggle).
// Hold (>= 0.7 s): GNOME's power-off dialog (org.gnome.SessionManager.Shutdown,
// answered by yupei-session-manager). logind is told to ignore the key
// (/etc/systemd/logind.conf.d/yupei.conf).
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

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void spawn(const char *cmd)
{
	fprintf(stderr, "yupei-powerkey: %s\n", cmd);
	if (fork() == 0) {
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

int main(void)
{
	int fd = -1;
	for (int i = 0; i < 64 && fd < 0; i++) {
		char p[32], name[64] = "";
		snprintf(p, sizeof p, "/dev/input/event%d", i);
		int f = open(p, O_RDONLY | O_CLOEXEC);
		if (f < 0)
			continue;
		ioctl(f, EVIOCGNAME(sizeof name), name);
		if (!strcmp(name, "pmic_pwrkey")) fd = f; else close(f);
	}
	if (fd < 0) {
		fprintf(stderr, "yupei-powerkey: pmic_pwrkey not found\n");
		return 1;
	}
	signal(SIGCHLD, SIG_IGN);
	long down = -1;
	int long_fired = 0;
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int timeout = down >= 0 && !long_fired ? (int)(LONG_MS - (now_ms() - down)) : -1;
		if (timeout < 0 && down >= 0 && !long_fired)
			timeout = 0;
		int r = poll(&p, 1, timeout);
		if (r == 0 && down >= 0 && !long_fired) {   // held long enough
			long_fired = 1;
			spawn("gdbus call --session --dest org.gnome.SessionManager "
			      "--object-path /org/gnome/SessionManager "
			      "--method org.gnome.SessionManager.Shutdown >/dev/null");
			continue;
		}
		struct input_event e;
		if (read(fd, &e, sizeof e) != sizeof e)
			continue;
		if (e.type != EV_KEY || e.code != KEY_POWER)
			continue;
		if (e.value == 1) {
			down = now_ms();
			long_fired = 0;
		} else if (e.value == 0 && down >= 0) {
			if (!long_fired)
				spawn("/usr/lib/yupei/screen-toggle");
			down = -1;
		}
	}
}
