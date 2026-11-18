#define _GNU_SOURCE /* accept4, struct ucred */
#include "output.h"
#include "profile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/uinput.h>

#define AXIS_RANGE 1024

/* Logical button index -> evdev code: BTN_0..BTN_9, then BTN_TRIGGER_HAPPY1.. */
static int button_code(int idx)
{
	if (idx < 10)
		return BTN_0 + idx;
	return BTN_TRIGGER_HAPPY1 + (idx - 10);
}

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = (unsigned short)type;
	ev.code = (unsigned short)code;
	ev.value = val;
	if (write(fd, &ev, sizeof ev) < 0) {
		/* drop event; uinput write only fails if device is gone */
	}
}

/* The keyboard half: only the keys any binding may send. -1 if none / failed. */
static int open_keyboard(const struct tdx_device_def *dev, const struct tdx_config *cfg)
{
	int any = 0;
	for (int i = 0; i < cfg->nkeymap && !any; i++)
		any = cfg->keymap[i].keys[0] != 0;
	if (!any)
		return -1;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	for (int i = 0; i < cfg->nkeymap; i++)
		for (int k = 0; k < TDX_MAX_KEYS && cfg->keymap[i].keys[k]; k++)
			ioctl(fd, UI_SET_KEYBIT, cfg->keymap[i].keys[k]);
	/* and the whole keyboard (Esc .. F24), so any profile, including the
	 * imported 3DxWare ones, can send its keys without a new device; not the
	 * power keys (116, 142, 143), so the desktop doesn't take it for a power button */
	for (int code = KEY_ESC; code <= KEY_F24; code++)
		if (code != KEY_POWER && code != KEY_SLEEP && code != KEY_WAKEUP)
			ioctl(fd, UI_SET_KEYBIT, code);
	struct uinput_setup s;
	memset(&s, 0, sizeof s);
	s.id.bustype = BUS_VIRTUAL;
	s.id.vendor = dev ? dev->vid : 0;
	s.id.product = dev ? dev->pid : 0;
	s.id.version = 1;
	snprintf(s.name, sizeof s.name, "3Dconnexion %s (tdxd keys)", dev ? dev->name : "device");
	if (ioctl(fd, UI_DEV_SETUP, &s) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int tdx_uinput_open(struct tdx_uinput *u, const struct tdx_device_def *dev,
		    const struct tdx_config *cfg)
{
	u->kfd = -1;
	u->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (u->fd < 0)
		return -errno;
	int nb = dev && dev->nbuttons ? dev->nbuttons : 32;
	if (nb > 50) nb = 50; /* BTN_TRIGGER_HAPPY1..40 + BTN_0..9 */
	u->nbuttons = nb;

	ioctl(u->fd, UI_SET_EVBIT, EV_ABS);
	ioctl(u->fd, UI_SET_EVBIT, EV_KEY);
	ioctl(u->fd, UI_SET_EVBIT, EV_SYN);
	static const int abs_codes[6] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ};
	for (int i = 0; i < 6; i++) {
		struct uinput_abs_setup a = {
			.code = (unsigned short)abs_codes[i],
			.absinfo = {.minimum = -AXIS_RANGE, .maximum = AXIS_RANGE, .flat = 0},
		};
		ioctl(u->fd, UI_SET_ABSBIT, abs_codes[i]);
		ioctl(u->fd, UI_ABS_SETUP, &a);
	}
	for (int i = 0; i < nb; i++)
		ioctl(u->fd, UI_SET_KEYBIT, button_code(i));

	struct uinput_setup s;
	memset(&s, 0, sizeof s);
	s.id.bustype = BUS_VIRTUAL;
	s.id.vendor = dev ? dev->vid : 0;
	s.id.product = dev ? dev->pid : 0;
	s.id.version = 1;
	snprintf(s.name, sizeof s.name, "3Dconnexion %s (tdxd)", dev ? dev->name : "device");
	if (ioctl(u->fd, UI_DEV_SETUP, &s) < 0 || ioctl(u->fd, UI_DEV_CREATE) < 0) {
		int e = -errno;
		close(u->fd);
		u->fd = -1;
		return e;
	}
	u->kfd = open_keyboard(dev, cfg);
	return 0;
}

void tdx_uinput_axes(struct tdx_uinput *u, const int axes[6])
{
	static const int abs_codes[6] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ};
	if (u->fd < 0)
		return;
	for (int i = 0; i < 6; i++) {
		int v = axes[i];
		if (v > AXIS_RANGE) v = AXIS_RANGE;
		if (v < -AXIS_RANGE) v = -AXIS_RANGE;
		emit(u->fd, EV_ABS, abs_codes[i], v);
	}
	emit(u->fd, EV_SYN, SYN_REPORT, 0);
}

void tdx_uinput_button(struct tdx_uinput *u, int idx, int pressed)
{
	if (u->fd < 0 || idx < 0 || idx >= u->nbuttons)
		return;
	emit(u->fd, EV_KEY, button_code(idx), pressed);
	emit(u->fd, EV_SYN, SYN_REPORT, 0);
}

void tdx_uinput_keys(struct tdx_uinput *u, const struct tdx_keymap *km, int pressed)
{
	int fd = u->kfd;  /* the keyboard device */
	if (fd < 0)
		return;
	const int *keys = km->keys;
	int n = 0;
	while (n < TDX_MAX_KEYS && keys[n])
		n++;
	if (km->seq) { /* tap each chord in turn when the button goes down */
		if (!pressed)
			return;
		for (int a = 0; a < n;) {
			int b = a;
			while (b < n && km->step[b] == km->step[a])
				b++;
			for (int k = a; k < b; k++)
				emit(fd, EV_KEY, keys[k], 1);
			emit(fd, EV_SYN, SYN_REPORT, 0);
			for (int k = b - 1; k >= a; k--)
				emit(fd, EV_KEY, keys[k], 0);
			emit(fd, EV_SYN, SYN_REPORT, 0);
			a = b;
		}
		return;
	}
	if (pressed) {
		for (int k = 0; k < n; k++)
			emit(fd, EV_KEY, keys[k], 1);
	} else {
		for (int k = n - 1; k >= 0; k--)
			emit(fd, EV_KEY, keys[k], 0);
	}
	emit(fd, EV_SYN, SYN_REPORT, 0);
}

void tdx_uinput_close(struct tdx_uinput *u)
{
	if (u->fd >= 0) {
		ioctl(u->fd, UI_DEV_DESTROY);
		close(u->fd);
	}
	if (u->kfd >= 0) {
		ioctl(u->kfd, UI_DEV_DESTROY);
		close(u->kfd);
	}
	u->fd = u->kfd = -1;
}

/*
 * spacenavd v0 wire format: every event is 8 native ints.
 *   motion:  {0, x, y, z, rx, ry, rz, period_ms}
 *   button:  {1 (press) | 2 (release), bnum, 0...}
 */
int tdx_spnav_open(struct tdx_spnav *s, const char *path)
{
	for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++)
		s->cfd[i] = -1;
	snprintf(s->path, sizeof s->path, "%s", path);
	s->lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s->lfd < 0)
		return -errno;
	struct sockaddr_un a;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	unlink(path);
	if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(s->lfd, 8) < 0) {
		int e = -errno;
		close(s->lfd);
		s->lfd = -1;
		return e;
	}
	chmod(path, 0666);
	return 0;
}

void tdx_spnav_accept(struct tdx_spnav *s)
{
	int fd = accept4(s->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return;
	for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++)
		if (s->cfd[i] < 0) {
			s->cfd[i] = fd;
			s->app[i][0] = 0;
			s->seq[i] = ++s->next_seq;
			struct ucred cr;
			socklen_t len = sizeof cr;
			if (!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) && cr.pid > 0)
				tdx_app_from_pid(cr.pid, s->app[i], sizeof s->app[i]);
			return;
		}
	close(fd);
}

const char *tdx_spnav_latest_app(const struct tdx_spnav *s)
{
	int best = -1;
	for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++)
		if (s->cfd[i] >= 0 && s->app[i][0] && (best < 0 || s->seq[i] > s->seq[best]))
			best = i;
	return best < 0 ? NULL : s->app[best];
}

void tdx_spnav_client_input(struct tdx_spnav *s, int idx)
{
	char buf[256];
	ssize_t n = read(s->cfd[idx], buf, sizeof buf);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		close(s->cfd[idx]);
		s->cfd[idx] = -1;
	}
	/* v0 clients send nothing meaningful; requests are ignored. */
}

static void broadcast(struct tdx_spnav *s, const int ev[8])
{
	for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++) {
		if (s->cfd[i] < 0)
			continue;
		if (send(s->cfd[i], ev, 8 * sizeof(int), MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
			close(s->cfd[i]);
			s->cfd[i] = -1;
		}
	}
}

void tdx_spnav_motion(struct tdx_spnav *s, const int axes[6], int period_ms)
{
	int ev[8] = {0, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5], period_ms};
	broadcast(s, ev);
}

void tdx_spnav_button(struct tdx_spnav *s, int bnum, int pressed)
{
	int ev[8] = {pressed ? 1 : 2, bnum, 0, 0, 0, 0, 0, 0};
	broadcast(s, ev);
}

void tdx_spnav_close(struct tdx_spnav *s)
{
	for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++)
		if (s->cfd[i] >= 0)
			close(s->cfd[i]);
	if (s->lfd >= 0) {
		close(s->lfd);
		unlink(s->path);
	}
	s->lfd = -1;
}
