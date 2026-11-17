/*
 * tdxd - Linux user-space driver for 3Dconnexion devices (SpacePilot Pro focus).
 *
 * Replaces, on Linux, the pieces of 3DxWare 10.9.14 that talk to hardware:
 *   3dxhid.sys + 3DxService.exe   -> hidraw reader, axis/button processing,
 *                                    uinput + spacenavd-protocol outputs
 *   LCDMon/EDMBTDD/LGPBTDD.sys    -> usbfs LCD driver (SpacePilot Pro QVGA)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/hidraw.h>

#include "config.h"
#include "gfx.h"
#include "hidraw.h"
#include "lcd.h"
#include "lcd_ent.h"
#include "lcd_giotto.h"
#include "control.h"
#include "output.h"
#include "profile.h"
#include "process.h"
#include "report.h"

#define MAX_DEV 8
#define DEFAULT_CONF "/etc/tdxd.conf"

struct dev_ctx {
	struct tdx_hid_node node;
	int fd;
	struct tdx_state st;
	struct tdx_uinput ui;
	struct tdx_lcd *lcd;
	struct tdx_entlcd *elcd;
	struct tdx_giolcd *glcd;
	uint32_t *gio_pages[TDX_GIO_MAX_PAGES]; /* rendered slide pages (Giotto) */
	int gio_npages;
	int slide_x, slide_target;              /* slide position / goal in pixels, -1 = idle */
	long long slide_next_ms;
	int lcd_page;
	int lcd_dirty;
	long long last_motion_ms;
	/* What was last sent, so a profile switch or reload only talks to the
	 * device when something changed: a HID SET_REPORT the device does not
	 * answer blocks write() for the 5 s timeout (and for good over a broken
	 * usbip link), stalling every client. 0 = nothing sent yet. */
	uint32_t led_sent, intensity_sent, bright_sent;
};

static void lcd_brightness_apply(struct dev_ctx *d);

/* FNV-1a over a report, never 0 (0 means "not sent"). */
static uint32_t report_sig(const void *p, size_t n, uint32_t h)
{
	const uint8_t *b = p;
	if (!h)
		h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h ? h : 1;
}

static volatile sig_atomic_t g_quit, g_reload;
static struct tdx_config cfg;
static const char *conf_path = DEFAULT_CONF;
static struct dev_ctx devs[MAX_DEV];
static int ndev;
static struct tdx_spnav spnav = {.lfd = -1};

/* per-application profiles */
static int cli_verbose;
static struct tdx_profiles profiles;
static int active_profile = -1;   /* index into profiles, -1 = base configuration */
static char focus_app[64];        /* last application reported by tdx-focus / tdxctl */
static char info_pressed[128];    /* LCDCfg.xml InfoLine: the button just pressed */
static int info_slider = -1;      /* ... and the speed after +/- */
static int lcdcfg_dirty;
static long long info_until;      /* when to clear them again */
static int focus_pid;
static int focus_known;           /* a focus helper has reported at least once */
static int ctl_fd = -1;
static int lcdmgr_fd = -1; /* held "lcd-release" connection from tdx-lcdmgr */
static char ctl_path[108];
static int key_union[3 * TDX_MAX_KEYMAP];
static int nkey_union;

static void on_sig(int s) { if (s == SIGHUP) g_reload = 1; else g_quit = 1; }

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define LOG(lvl, ...) do { if (cfg.verbose >= (lvl)) fprintf(stderr, "tdxd: " __VA_ARGS__); } while (0)

static void entlcd_redraw(struct dev_ctx *d)
{
	static uint32_t px[TDX_ENT_PIXELS];
	struct tdx_fb fb = {TDX_ENT_W, TDX_ENT_H, px};
	if (!cfg.lcd_image[0] || gfx_load_ppm(&fb, cfg.lcd_image) != 0)
		tdx_render_ent_page(&fb, &cfg, d->node.def->name, 0, 1);
	d->elcd->swap_rb = cfg.lcd_swap_rb;
	int rc = tdx_entlcd_show(d->elcd, px, 1);
	d->lcd_dirty = rc == -EBUSY;
	if (rc && rc != -EBUSY)
		LOG(0, "Enterprise LCD update failed: %s\n", strerror(-rc));
}

/* Render every configured label page; the slide composes from these on the device. */
static void gio_render_pages(struct dev_ctx *d)
{
	int np = cfg.lcd_image[0] ? 1 : tdx_config_lcd_pages(&cfg);
	for (int p = 0; p < TDX_GIO_MAX_PAGES; p++) {
		if (p >= np) {
			free(d->gio_pages[p]);
			d->gio_pages[p] = NULL;
			continue;
		}
		if (!d->gio_pages[p] && !(d->gio_pages[p] = malloc(TDX_GIO_PIXELS * sizeof(uint32_t)))) {
			np = p;
			break;
		}
		struct tdx_fb fb = {TDX_GIO_W, TDX_GIO_H, d->gio_pages[p]};
		if (!cfg.lcd_image[0] || gfx_load_ppm(&fb, cfg.lcd_image) != 0)
			tdx_render_ent_page(&fb, &cfg, d->node.def->name, p, np);
	}
	d->gio_npages = np;
	if (np)
		tdx_giolcd_set_pages(d->glcd, (const uint32_t *const *)d->gio_pages, np);
	if (d->lcd_page >= np)
		d->lcd_page = 0;
	d->slide_target = -1;
}

static void giolcd_redraw(struct dev_ctx *d)
{
	d->glcd->swap_rb = cfg.lcd_swap_rb;
	gio_render_pages(d);
	if (!d->gio_npages)
		return;
	/* full static frame (title bar included) of the current page */
	int rc = tdx_giolcd_show(d->glcd, d->gio_pages[d->lcd_page]);
	d->slide_x = d->lcd_page * TDX_GIO_W;
	if (rc)
		LOG(0, "Giotto LCD update failed: %s\n", strerror(-rc));
}

/* Advance a running slide by one 25 ms step (called from the main loop). */
static void gio_slide_tick(struct dev_ctx *d)
{
	if (!d->glcd || d->slide_target < 0)
		return;
	long long t = now_ms();
	if (t < d->slide_next_ms)
		return;
	d->slide_next_ms = t + TDX_GIO_SLIDE_STEP_MS; /* Windows: Sleep(25) per step */
	int step = cfg.lcd_slide_step > 0 ? cfg.lcd_slide_step : 80;
	int x = d->slide_x;
	if (d->slide_target > x)
		x = x + step > d->slide_target ? d->slide_target : x + step;
	else
		x = x - step < d->slide_target ? d->slide_target : x - step;
	d->slide_x = x;
	int rc = tdx_giolcd_slide(d->glcd, x);
	if (rc) {
		LOG(0, "Giotto LCD slide failed: %s\n", strerror(-rc));
		d->slide_target = -1;
		return;
	}
	if (x == d->slide_target) {
		/* finished: the slide swapped buffers without syncing them, so copy the
		 * front into the back, then rewrite only the title bar (page dots) with a
		 * 0x21 rectangle write instead of a full frame */
		d->lcd_page = x / TDX_GIO_W;
		d->slide_target = -1;
		rc = tdx_giolcd_sync_back(d->glcd);
		if (!rc)
			rc = tdx_giolcd_show_rect(d->glcd, d->gio_pages[d->lcd_page], 0, 0, TDX_GIO_W,
						  TDX_GIO_SLIDE_Y);
		if (rc)
			LOG(0, "Giotto LCD update failed: %s\n", strerror(-rc));
	}
}

/* Start sliding to the next label page (wrapping back to the first). */
static void gio_slide_next(struct dev_ctx *d)
{
	if (!d->glcd || d->gio_npages < 2 || d->slide_target >= 0)
		return;
	int next = (d->lcd_page + 1) % d->gio_npages;
	d->slide_target = next * TDX_GIO_W;
	LOG(2, "Giotto LCD: slide to page %d\n", next + 1);
}

static void lcd_redraw(struct dev_ctx *d)
{
	if (d->elcd) {
		entlcd_redraw(d);
		return;
	}
	if (d->glcd) {
		giolcd_redraw(d);
		return;
	}
	if (!d->lcd)
		return;
	static uint32_t px[TDX_LCD_W * TDX_LCD_H];
	struct tdx_fb fb = {TDX_LCD_W, TDX_LCD_H, px};
	if (!cfg.lcd_image[0] || gfx_load_ppm(&fb, cfg.lcd_image) != 0)
		tdx_render_keys_page(&fb, &cfg, d->node.def->name, d->lcd_page);
	int rc = tdx_lcd_show(d->lcd, px);
	if (rc && rc != -ENODEV) {
		/* e.g. after host suspend: wait for the panel like EDMBTDD does, retry once */
		LOG(1, "LCD write failed (%s), waiting for device\n", strerror(-rc));
		if (tdx_lcd_wait_ready(d->lcd, 20) == 0)
			rc = tdx_lcd_show(d->lcd, px);
	}
	if (rc)
		LOG(0, "LCD update failed: %s\n", strerror(-rc));
}

static void entlcd_attach(struct dev_ctx *d)
{
	d->elcd = calloc(1, sizeof *d->elcd);
	if (!d->elcd)
		return;
	int rc = tdx_entlcd_open(d->elcd, d->node.vid, d->node.pid);
	if (rc) {
		LOG(0, "Enterprise LCD unavailable (%s): %s\n",
		    d->elcd->path[0] ? d->elcd->path : "no usb node", strerror(-rc));
		free(d->elcd);
		d->elcd = NULL;
		return;
	}
	LOG(1, "Enterprise LCD on %s, bulk ep 0x%02x\n", d->elcd->path, d->elcd->ep_out);
	lcd_redraw(d); /* first frame: opcode 0x15, then 5 s init window */
	d->lcd_dirty = 1; /* redraw once the panel has finished initialising */
}

/*
 * Giotto legacy BMP path: HIDDev.dll's LCD thread watches a directory and, when
 * sp4LCD.bmp is (re)written, sends it with opcode 0x13 and deletes it
 * (FUN_180049c00 -> FUN_18004a8a0, DeleteFileW). Opt-in via lcd_bmp_dir.
 */
static int bmp_ifd = -1;

static void bmp_send_pending(void)
{
	if (!cfg.lcd_bmp_dir[0])
		return;
	char bmp[512], ctl[512];
	snprintf(bmp, sizeof bmp, "%s/sp4LCD.bmp", cfg.lcd_bmp_dir);
	snprintf(ctl, sizeof ctl, "%s/sp4LCDControl.txt", cfg.lcd_bmp_dir);
	if (access(bmp, F_OK))
		return;
	int sent = 0;
	for (int i = 0; i < ndev; i++) {
		if (!devs[i].glcd)
			continue;
		int rc = tdx_giolcd_send_bmp(devs[i].glcd, bmp, ctl);
		LOG(rc ? 0 : 1, "Giotto LCD: sp4LCD.bmp %s%s\n", rc ? "failed: " : "sent",
		    rc ? strerror(-rc) : "");
		sent = 1;
	}
	if (sent)
		unlink(bmp); /* Windows deletes the file after sending it */
}

static void bmp_watch_setup(void)
{
	if (bmp_ifd >= 0)
		close(bmp_ifd);
	bmp_ifd = -1;
	if (!cfg.lcd_bmp_dir[0])
		return;
	bmp_ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (bmp_ifd < 0 || inotify_add_watch(bmp_ifd, cfg.lcd_bmp_dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		LOG(0, "cannot watch %s: %s\n", cfg.lcd_bmp_dir, strerror(errno));
		if (bmp_ifd >= 0)
			close(bmp_ifd);
		bmp_ifd = -1;
	}
}

static void bmp_watch_read(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	int hit = 0;
	for (;;) {
		ssize_t n = read(bmp_ifd, buf, sizeof buf);
		if (n <= 0)
			break;
		for (char *p = buf; p < buf + n;) {
			const struct inotify_event *ev = (const struct inotify_event *)p;
			if (ev->len && !strcmp(ev->name, "sp4LCD.bmp"))
				hit = 1;
			p += sizeof *ev + ev->len;
		}
	}
	if (hit)
		bmp_send_pending();
}

static void lcd_attach(struct dev_ctx *d)
{
	if (!cfg.lcd_enable || d->node.bus != 3)
		return;
	if (lcdmgr_fd >= 0 && d->node.def->lcd == TDX_LCD_QVGA)
		return; /* tdx-lcdmgr owns the SpacePilot Pro LCD */
	if (d->node.def->lcd == TDX_LCD_ENT640) {
		entlcd_attach(d);
		return;
	}
	if (d->node.def->lcd == TDX_LCD_GIO1280) {
		d->glcd = calloc(1, sizeof *d->glcd);
		if (!d->glcd)
			return;
		int rc = tdx_giolcd_open(d->glcd, d->node.vid, d->node.pid);
		if (rc) {
			LOG(0, "Giotto LCD unavailable (%s): %s\n",
			    d->glcd->path[0] ? d->glcd->path : "no usb node", strerror(-rc));
			free(d->glcd);
			d->glcd = NULL;
			return;
		}
		LOG(1, "Giotto LCD on %s, interface %d, bulk ep 0x%02x\n", d->glcd->path,
		    d->glcd->iface, d->glcd->ep_out);
		lcd_redraw(d);
		bmp_send_pending(); /* like Windows' first CompareTimes: send a file already there */
		return;
	}
	if (d->node.def->lcd != TDX_LCD_QVGA)
		return;
	d->lcd = calloc(1, sizeof *d->lcd);
	if (!d->lcd)
		return;
	int rc = tdx_lcd_open(d->lcd, d->node.vid, d->node.pid);
	if (rc) {
		LOG(0, "LCD unavailable (%s): %s\n", d->lcd->path[0] ? d->lcd->path : "no usb node",
		    strerror(-rc));
		free(d->lcd);
		d->lcd = NULL;
		return;
	}
	LOG(1, "LCD on %s, bulk ep 0x%02x\n", d->lcd->path, d->lcd->ep_out);
	rc = tdx_lcd_wait_ready(d->lcd, 20);
	if (rc)
		LOG(0, "LCD did not answer ping (%s); trying anyway\n", strerror(-rc));
	d->bright_sent = 0; /* a (re)opened LCD gets the brightness again */
	lcd_brightness_apply(d);
	lcd_redraw(d);
}

/* Enterprise LCD brightness: feature report {0x11, percent} (HIDDev.dll case 8). */
static void lcd_intensity_apply(struct dev_ctx *d)
{
	uint8_t rep[2];
	if (!tdx_lcd_intensity_report(d->node.def, cfg.lcd_brightness, rep))
		return;
	uint32_t sig = report_sig(rep, sizeof rep, 0);
	if (sig == d->intensity_sent)
		return;
	if (ioctl(d->fd, HIDIOCSFEATURE(2), rep) < 0) {
		LOG(1, "LCD intensity write failed on %s: %s\n", d->node.devnode, strerror(errno));
	} else {
		d->intensity_sent = sig;
		LOG(2, "LCD intensity %d%% on %s\n", rep[1], d->node.devnode);
	}
}

/* SpacePilot PRO LCD backlight, only when it changed (or the LCD was reopened). */
static void lcd_brightness_apply(struct dev_ctx *d)
{
	if (!d->lcd || cfg.lcd_brightness < 0)
		return;
	uint32_t sig = (uint32_t)cfg.lcd_brightness + 1;
	if (sig == d->bright_sent)
		return;
	if (tdx_lcd_set_brightness(d->lcd, cfg.lcd_brightness) == 0)
		d->bright_sent = sig;
}

static void led_apply(struct dev_ctx *d)
{
	if (tdx_has_rgb_leds(d->node.def)) {
		uint8_t reps[TDX_RGB_RING_FIRST + TDX_RGB_NUM_RING + 2][8];
		int n = tdx_rgb_plan(&cfg, d->node.def, reps, (int)(sizeof reps / sizeof reps[0]));
		if (n <= 0)
			return;
		uint32_t sig = report_sig(reps, (size_t)n * sizeof reps[0], 0);
		if (sig == d->led_sent)
			return;
		for (int i = 0; i < n; i++)
			if (ioctl(d->fd, HIDIOCSFEATURE(8), reps[i]) < 0) {
				LOG(1, "LED colour write failed on %s: %s\n", d->node.devnode,
				    strerror(errno));
				return;
			}
		d->led_sent = sig;
		LOG(2, "sent %d LED colour report(s)\n", n);
		return;
	}
	uint8_t rep[2];
	if (cfg.led < 0 || !tdx_led_report(d->node.def, cfg.led, rep))
		return;
	uint32_t sig = report_sig(rep, sizeof rep, 0);
	if (sig == d->led_sent)
		return;
	if (write(d->fd, rep, sizeof rep) != (ssize_t)sizeof rep)
		LOG(1, "LED write failed on %s: %s\n", d->node.devnode, strerror(errno));
	else
		d->led_sent = sig;
}

static void dev_close(struct dev_ctx *d)
{
	LOG(0, "device removed: %s (%s)\n", d->node.def->name, d->node.devnode);
	if (d->fd >= 0)
		close(d->fd);
	tdx_uinput_close(&d->ui);
	if (d->lcd) {
		tdx_lcd_close(d->lcd);
		free(d->lcd);
	}
	if (d->elcd) {
		tdx_entlcd_close(d->elcd);
		free(d->elcd);
	}
	if (d->glcd) {
		tdx_giolcd_close(d->glcd);
		free(d->glcd);
	}
	for (int p = 0; p < TDX_GIO_MAX_PAGES; p++)
		free(d->gio_pages[p]);
	*d = devs[--ndev];
}

/* ---- calibration (HIDDev.dll UdDevRezero) ---- */

static int device_rezero(struct dev_ctx *d)
{
	uint8_t rep[2];
	if (!tdx_rezero_report(d->node.def, d->node.version, rep)) {
		LOG(0, "%s: firmware %x.%02x does not support re-zero (3DxWare skips it too)\n",
		    d->node.def->name, d->node.version >> 8, d->node.version & 0xFF);
		return -ENOTSUP;
	}
	if (ioctl(d->fd, HIDIOCSFEATURE(2), rep) < 0) {
		int e = errno;
		LOG(0, "%s: re-zero failed: %s\n", d->node.def->name, strerror(e));
		return -e;
	}
	LOG(0, "%s: re-zeroed (rest position calibrated)\n", d->node.def->name);
	return 0;
}

/* ---- per-application profiles ---- */

static void apply_effective(int idx)
{
	tdx_config_compose(&cfg, conf_path, &profiles, idx);
	if (cli_verbose)
		cfg.verbose = cli_verbose;
	active_profile = idx;
}

/* Base config, profile list, and the union of all keys any profile can send. */
static void load_all(void)
{
	tdx_config_compose(&cfg, conf_path, &profiles, -1); /* profile_dir lives in the base */
	int rc = tdx_profiles_scan(cfg.profile_dir, &profiles);
	if (rc)
		fprintf(stderr, "tdxd: cannot read %s: %s\n", cfg.profile_dir, strerror(-rc));
	nkey_union = tdx_profiles_key_union(conf_path, &profiles, key_union,
					    (int)(sizeof key_union / sizeof key_union[0]));
	int idx = active_profile < profiles.n ? active_profile : -1;
	apply_effective(idx);
}

static const char *profile_name(int idx)
{
	return idx >= 0 ? profiles.p[idx].name : "default";
}

/* The application profiles follow: what tdx-focus reported, else the newest spnav client. */
static const char *current_app(const char **source)
{
	if (focus_known) {
		*source = "focus";
		return focus_app;
	}
	const char *a = spnav.lfd >= 0 ? tdx_spnav_latest_app(&spnav) : NULL;
	*source = a ? "spnav-client" : "none";
	return a;
}

/* LCDCfg.xml for the LCD applets (Function Keys), as 3DxService writes it. */
static void lcdcfg_write(void)
{
	if (!cfg.lcd_cfg[0] || !ndev)
		return;
	const struct dev_ctx *d = &devs[0];
	for (int i = 0; i < ndev; i++)
		if (devs[i].node.def->lcd) {
			d = &devs[i];
			break;
		}
	const char *src;
	const char *app = current_app(&src);
	char exe[256] = "";
	if (focus_known && focus_pid > 0) {
		char p[64];
		snprintf(p, sizeof p, "/proc/%d/exe", focus_pid);
		ssize_t n = readlink(p, exe, sizeof exe - 1);
		exe[n > 0 ? n : 0] = 0;
	}
	struct tdx_lcdcfg_info info = {
		.app = app ? app : profile_name(active_profile), .app_image = exe,
		.pressed = info_pressed, .slider = info_slider,
		.lcd_level = cfg.lcd_brightness < 0 ? 2 : cfg.lcd_brightness < 40 ? 0
			   : cfg.lcd_brightness < 75 ? 1 : 2,
		.led = cfg.led == 0 ? 0 : 100,
	};
	static char xml[16384];
	int n = tdx_lcdcfg_xml(xml, sizeof xml, &cfg, d->node.def, d->node.vid, d->node.pid, &info);
	if (n < 0)
		return;
	char dir[256], tmp[300];
	snprintf(dir, sizeof dir, "%s", cfg.lcd_cfg);
	char *slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = 0;
		mkdir(dir, 0755);
	}
	snprintf(tmp, sizeof tmp, "%s.tmp", cfg.lcd_cfg);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	int ok = write(fd, xml, (size_t)n) == n;
	close(fd);
	if (ok)
		rename(tmp, cfg.lcd_cfg);
	else
		unlink(tmp);
}

static void refresh_devices(void)
{
	for (int i = 0; i < ndev; i++) {
		lcd_brightness_apply(&devs[i]);
		led_apply(&devs[i]);
		lcd_intensity_apply(&devs[i]);
		lcd_redraw(&devs[i]);
	}
	lcdcfg_write();
}

static void select_profile(void)
{
	const char *src;
	const char *app = current_app(&src);
	int idx = tdx_profile_match(&profiles, app);
	if (idx == active_profile)
		return;
	apply_effective(idx);
	LOG(0, "profile: %s (application %s, via %s)\n", profile_name(idx), app ? app : "-", src);
	refresh_devices();
}

/* uinput gets every key any profile may send, so profile switches need no new device. */
static void uinput_open_dev(struct dev_ctx *d)
{
	struct tdx_config *u = malloc(sizeof *u);
	if (!u)
		return;
	*u = cfg;
	u->nkeymap = 0;
	for (int i = 0; i < nkey_union; i += 3) {
		struct tdx_keymap *m = &u->keymap[u->nkeymap++];
		memset(m, 0, sizeof *m);
		for (int k = 0; k < 3 && i + k < nkey_union; k++)
			m->keys[k] = key_union[i + k];
	}
	int rc = tdx_uinput_open(&d->ui, d->node.def, u);
	free(u);
	if (rc)
		LOG(0, "uinput disabled: %s\n", strerror(-rc));
}

static void ctl_handler(char *cmd, char *reply, size_t len)
{
	char *save = NULL, *verb = strtok_r(cmd, " \t", &save);
	if (!verb) {
		snprintf(reply, len, "error: empty command\n");
	} else if (!strcmp(verb, "focus")) {
		char *app = strtok_r(NULL, " \t", &save), *pid = strtok_r(NULL, " \t", &save);
		if (!app) {
			snprintf(reply, len, "error: focus needs an application name\n");
			return;
		}
		snprintf(focus_app, sizeof focus_app, "%s", app);
		focus_pid = pid ? atoi(pid) : 0;
		focus_known = 1;
		select_profile();
		lcdcfg_write(); /* the application name changed even if the profile didn't */
		snprintf(reply, len, "ok profile=%s\n", profile_name(active_profile));
	} else if (!strcmp(verb, "calibrate")) {
		size_t o = 0;
		if (!ndev)
			o += (size_t)snprintf(reply, len, "error: no device\n");
		for (int i = 0; i < ndev && o < len; i++) {
			int rc = device_rezero(&devs[i]);
			o += (size_t)snprintf(reply + o, len - o, "%s %s: %s\n", rc ? "error" : "ok",
					      devs[i].node.def->name,
					      rc ? strerror(-rc) : "re-zeroed");
		}
	} else if (!strcmp(verb, "lcd-release")) {
		if (lcdmgr_fd >= 0) {
			snprintf(reply, len, "error: the LCD is already managed\n");
			return;
		}
		int n = 0;
		for (int i = 0; i < ndev; i++)
			if (devs[i].lcd) {
				tdx_lcd_close(devs[i].lcd);
				free(devs[i].lcd);
				devs[i].lcd = NULL;
				n++;
			}
		lcdmgr_fd = -2; /* set to the connection fd after the reply */
		tdx_ctl_keep_connection();
		LOG(0, "LCD handed to the LCD manager (%d released)\n", n);
		snprintf(reply, len, "ok lcd-released %d\n", n);
	} else if (!strcmp(verb, "reload")) {
		g_reload = 1;
		snprintf(reply, len, "ok reloading\n");
	} else if (!strcmp(verb, "profiles")) {
		size_t o = (size_t)snprintf(reply, len, "profile_dir %s (%d profiles)\n", cfg.profile_dir,
					    profiles.n);
		for (int i = 0; i < profiles.n && o < len; i++)
			o += (size_t)snprintf(reply + o, len - o, "%c %-20s match: %s\n",
					      i == active_profile ? '*' : ' ', profiles.p[i].name,
					      profiles.p[i].match);
	} else if (!strcmp(verb, "status")) {
		const char *src;
		const char *app = current_app(&src);
		size_t o = (size_t)snprintf(reply, len,
					    "tdxd %s\nprofile %s\napplication %s (via %s)\ndevices %d\n",
#ifdef TDX_VERSION
					    TDX_VERSION,
#else
					    "unknown",
#endif
					    profile_name(active_profile), app ? app : "-", src, ndev);
		o += (size_t)snprintf(reply + o, len - o,
				      "speed %.3f\npanzoom %s\nrotation %s\ndominant %s\nlcd %s\n",
				      cfg.sensitivity, cfg.disable_translation ? "off" : "on",
				      cfg.disable_rotation ? "off" : "on", cfg.dominant ? "on" : "off",
				      lcdmgr_fd >= 0 ? "lcdmgr" : "tdxd");
		for (int i = 0; i < ndev && o < len; i++)
			o += (size_t)snprintf(reply + o, len - o, "  %s [%04x:%04x] %s firmware %x.%02x\n",
					      devs[i].node.def->name, devs[i].node.vid,
					      devs[i].node.pid, devs[i].node.devnode,
					      devs[i].node.version >> 8, devs[i].node.version & 0xFF);
	} else {
		snprintf(reply, len, "error: unknown command '%s'\n", verb);
	}
}

static void rescan(void)
{
	struct tdx_hid_node nodes[MAX_DEV];
	int n = tdx_hidraw_scan(nodes, MAX_DEV);
	for (int i = 0; i < n && ndev < MAX_DEV; i++) {
		int known = 0;
		for (int j = 0; j < ndev; j++)
			if (!strcmp(devs[j].node.devnode, nodes[i].devnode))
				known = 1;
		if (known)
			continue;
		struct dev_ctx *d = &devs[ndev];
		memset(d, 0, sizeof *d);
		d->node = nodes[i];
		d->ui.fd = -1;
		d->slide_target = -1;
		d->fd = open(d->node.devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (d->fd < 0)
			d->fd = open(d->node.devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (d->fd < 0) {
			LOG(1, "cannot open %s: %s\n", d->node.devnode, strerror(errno));
			continue;
		}
		ndev++;
		LOG(0, "device: %s [%04x:%04x] on %s\n", d->node.def->name, d->node.vid,
		    d->node.pid, d->node.devnode);
		if (cfg.enable_uinput)
			uinput_open_dev(d);
		if (cfg.calibrate_on_start)
			device_rezero(d);
		led_apply(d);
		lcd_intensity_apply(d);
		lcd_attach(d);
		lcdcfg_dirty = 1;
	}
}

static void handle_buttons(struct dev_ctx *d, const uint8_t prev[TDX_MAX_BUTTONS])
{
	int n = tdx_button_count(d->node.def);
	for (int i = 0; i < n; i++) {
		if (prev[i] == d->st.btn[i])
			continue;
		int p = d->st.btn[i];
		const char *v3dk = d->node.def->nbuttons ? d->node.def->buttons[i].v3dk : NULL;
		LOG(2, "button %d (%s) %s\n", i + 1, v3dk ? v3dk : "-", p ? "down" : "up");
		if (p && cfg.lcd_cfg[0]) { /* LCDCfg.xml <ButtonPressed>, cleared after 3 s */
			tdx_button_text(&cfg, d->node.def, i, info_pressed, sizeof info_pressed);
			info_until = now_ms() + 3000;
			lcdcfg_dirty = 1;
		}
		int act = tdx_action_for(&cfg, d->node.def, i);
		if (act != TDX_ACT_NONE) {
			/* driver action: consumed here, like 3DxWare (not sent to applications) */
			if (!p)
				continue;
			if (tdx_apply_action(&cfg, act)) {
				LOG(0, "%s: speed %.2f, pan/zoom %s, rotation %s, dominant %s\n",
				    tdx_action_name(act), cfg.sensitivity,
				    cfg.disable_translation ? "off" : "on",
				    cfg.disable_rotation ? "off" : "on", cfg.dominant ? "on" : "off");
				if (act == TDX_ACT_SPEED_UP || act == TDX_ACT_SPEED_DOWN)
					info_slider = (int)(cfg.sensitivity * 100 + 0.5);
				info_until = now_ms() + 3000;
				lcdcfg_dirty = 1;
			}
			else if (act == TDX_ACT_CALIBRATE)
				device_rezero(d);
			else if (act == TDX_ACT_LCD_NEXT && d->glcd)
				gio_slide_next(d);
			continue;
		}
		const struct tdx_keymap *km = tdx_keymap_for(&cfg, d->node.def, i);
		if (km)
			tdx_uinput_keys(&d->ui, km->keys, p);
		else
			tdx_uinput_button(&d->ui, i, p);
		if (spnav.lfd >= 0)
			tdx_spnav_button(&spnav, i, p);
		/* LCD paging: SHIFT shows keys 6..10 while held */
		if (d->lcd && v3dk && !strcmp(v3dk, "V3DK_SHIFT")) {
			d->lcd_page = p ? 1 : 0;
			lcd_redraw(d);
		}
		/* "Calibrate" bound to a button (3DxWare's Driver_RezeroDevice action) */
		if (p && v3dk && cfg.calibrate_button[0] && !strcasecmp(v3dk, cfg.calibrate_button))
			device_rezero(d);
		/* Giotto: slide to the next label page */
		if (p && d->glcd && v3dk && cfg.lcd_slide_button[0] &&
		    !strcasecmp(v3dk, cfg.lcd_slide_button))
			gio_slide_next(d);
	}
}

static void dev_read(struct dev_ctx *d)
{
	uint8_t buf[64];
	for (;;) {
		ssize_t n = read(d->fd, buf, sizeof buf);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return;
			dev_close(d);
			return;
		}
		if (n == 0)
			return;
		uint8_t prev[TDX_MAX_BUTTONS];
		memcpy(prev, d->st.btn, sizeof prev);
		int r = tdx_report_feed(&d->st, d->node.def, buf, (size_t)n);
		if (r & 1) {
			int out[6];
			tdx_process_axes(&cfg, d->st.axis, out);
			tdx_uinput_axes(&d->ui, out);
			long long t = now_ms();
			int period = d->last_motion_ms ? (int)(t - d->last_motion_ms) : 0;
			d->last_motion_ms = t;
			if (spnav.lfd >= 0)
				tdx_spnav_motion(&spnav, out, period);
		}
		if (r & 2)
			handle_buttons(d, prev);
	}
}

static void usage(void)
{
	puts("usage: tdxd [-c config] [-v]... [-l] [-V]\n"
	     "  -c FILE  configuration file (default " DEFAULT_CONF ")\n"
	     "  -v       more logging (repeatable)\n"
	     "  -l       list supported devices and exit\n"
	     "  -V       print version and exit");
}

#ifndef TDX_VERSION
#define TDX_VERSION "unknown"
#endif

int main(int argc, char **argv)
{
	int opt, verbose = 0;
	while ((opt = getopt(argc, argv, "c:vlhV")) != -1) {
		switch (opt) {
		case 'V': puts("tdxd " TDX_VERSION); return 0;
		case 'c': conf_path = optarg; break;
		case 'v': verbose++; break;
		case 'l':
			for (size_t i = 0; i < tdx_device_count; i++) {
				const char *lcd = tdx_devices[i].lcd == TDX_LCD_QVGA     ? "  lcd=320x240"
						  : tdx_devices[i].lcd == TDX_LCD_ENT640  ? "  lcd=640x150"
						  : tdx_devices[i].lcd == TDX_LCD_GIO1280 ? "  lcd=1280x416"
											  : "";
				printf("%04x:%04x  %-34s buttons=%-3d%s\n", tdx_devices[i].vid,
				       tdx_devices[i].pid, tdx_devices[i].name, tdx_devices[i].nbuttons, lcd);
			}
			return 0;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}

	cli_verbose = verbose;
	if (access(conf_path, R_OK) && strcmp(conf_path, DEFAULT_CONF))
		fprintf(stderr, "tdxd: cannot read %s: %s\n", conf_path, strerror(errno));
	load_all();
	if (profiles.n)
		LOG(0, "%d per-application profile(s) in %s\n", profiles.n, cfg.profile_dir);

	struct sigaction sa = {.sa_handler = on_sig};
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (cfg.enable_spnav) {
		int rc = tdx_spnav_open(&spnav, cfg.spnav_socket);
		if (rc)
			LOG(0, "spnav socket %s: %s (disabled)\n", cfg.spnav_socket, strerror(-rc));
	}
	bmp_watch_setup();
	if (cfg.control_socket[0]) {
		snprintf(ctl_path, sizeof ctl_path, "%s", cfg.control_socket);
		ctl_fd = tdx_ctl_listen(ctl_path);
		if (ctl_fd < 0)
			LOG(0, "control socket %s: %s (tdxctl disabled)\n", ctl_path, strerror(-ctl_fd));
	}

	long long last_scan = 0;
	while (!g_quit) {
		if (g_reload) {
			g_reload = 0;
			load_all();
			active_profile = -2; /* force re-selection against the new profile list */
			select_profile();
			LOG(0, "configuration reloaded (%d profiles, active %s)\n", profiles.n,
			    profile_name(active_profile));
			for (int i = 0; i < ndev; i++) { /* key bindings may have changed */
				tdx_uinput_close(&devs[i].ui);
				if (cfg.enable_uinput)
					uinput_open_dev(&devs[i]);
			}
			refresh_devices();
			bmp_watch_setup();
			bmp_send_pending();
		}
		if (now_ms() - last_scan > 2000) {
			rescan();
			last_scan = now_ms();
		}

		struct pollfd pfd[MAX_DEV + 4 + TDX_SPNAV_MAX_CLIENTS];
		int np = 0;
		for (int i = 0; i < ndev; i++)
			pfd[np++] = (struct pollfd){.fd = devs[i].fd, .events = POLLIN};
		int ctl_idx = -1;
		if (ctl_fd >= 0) {
			ctl_idx = np;
			pfd[np++] = (struct pollfd){.fd = ctl_fd, .events = POLLIN};
		}
		int mgr_idx = -1;
		if (lcdmgr_fd >= 0) {
			mgr_idx = np;
			pfd[np++] = (struct pollfd){.fd = lcdmgr_fd, .events = POLLIN};
		}
		int bmp_idx = -1;
		if (bmp_ifd >= 0) {
			bmp_idx = np;
			pfd[np++] = (struct pollfd){.fd = bmp_ifd, .events = POLLIN};
		}
		int sp_base = np;
		if (spnav.lfd >= 0) {
			pfd[np++] = (struct pollfd){.fd = spnav.lfd, .events = POLLIN};
			for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++)
				pfd[np++] = (struct pollfd){.fd = spnav.cfd[i], .events = POLLIN};
		}
		int any_dirty = 0, any_slide = 0;
		for (int i = 0; i < ndev; i++) {
			any_dirty |= devs[i].lcd_dirty;
			any_slide |= devs[i].slide_target >= 0 && devs[i].glcd;
		}
		int r = poll(pfd, (nfds_t)np,
			     any_slide ? TDX_GIO_SLIDE_STEP_MS : any_dirty ? 250 : 1000);
		for (int i = 0; i < ndev; i++)
			gio_slide_tick(&devs[i]);
		for (int i = 0; i < ndev; i++) /* Enterprise LCD finished its init window? */
			if (devs[i].lcd_dirty)
				lcd_redraw(&devs[i]);
		if (info_until && now_ms() >= info_until) { /* InfoLine shown long enough */
			info_until = 0;
			info_pressed[0] = 0;
			info_slider = -1;
			lcdcfg_dirty = 1;
		}
		if (r <= 0) {
			if (lcdcfg_dirty) {
				lcdcfg_dirty = 0;
				lcdcfg_write();
			}
			continue;
		}
		if (bmp_idx >= 0 && (pfd[bmp_idx].revents & POLLIN))
			bmp_watch_read();
		if (mgr_idx >= 0 && (pfd[mgr_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
			char junk[64];
			if (read(lcdmgr_fd, junk, sizeof junk) <= 0) { /* manager gone: take the LCD back */
				close(lcdmgr_fd);
				lcdmgr_fd = -1;
				LOG(0, "LCD manager disconnected; tdxd drives the LCD again\n");
				for (int i = 0; i < ndev; i++) {
					lcd_attach(&devs[i]);
					lcd_redraw(&devs[i]);
				}
			}
		}
		if (ctl_idx >= 0 && (pfd[ctl_idx].revents & POLLIN)) {
			int kept = tdx_ctl_serve(ctl_fd, ctl_handler);
			if (lcdmgr_fd == -2)
				lcdmgr_fd = kept; /* -1 if the client vanished: back to tdxd */
			if (lcdmgr_fd == -1 && kept < 0) {
				for (int i = 0; i < ndev; i++)
					if (!devs[i].lcd && devs[i].node.def->lcd == TDX_LCD_QVGA) {
						lcd_attach(&devs[i]);
						lcd_redraw(&devs[i]);
					}
			}
		}
		for (int i = ndev - 1; i >= 0; i--)
			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
				dev_read(&devs[i]);
		if (spnav.lfd >= 0) {
			if (pfd[sp_base].revents & POLLIN)
				tdx_spnav_accept(&spnav);
			for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++)
				if (spnav.cfd[i] >= 0 && (pfd[sp_base + 1 + i].revents & (POLLIN | POLLHUP)))
					tdx_spnav_client_input(&spnav, i);
			if (!focus_known) /* no focus helper: follow the newest spnav client */
				select_profile();
		}
		if (lcdcfg_dirty) {
			lcdcfg_dirty = 0;
			lcdcfg_write();
		}
	}

	while (ndev)
		dev_close(&devs[ndev - 1]);
	tdx_spnav_close(&spnav);
	if (ctl_fd >= 0) {
		close(ctl_fd);
		unlink(ctl_path);
	}
	return 0;
}
