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
#include <pwd.h>
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
#include <sys/wait.h>
#include <linux/hidraw.h>
#include <linux/input-event-codes.h>

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
#include "receiver.h"
#include "report.h"

#define MAX_DEV 8
#define MAX_RCV 10 /* receiver interfaces (a Universal Receiver has five) */
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

/* One interface of a wireless receiver. Interface N carries the device in
 * pairing slot N; its input reports (mouse motion, buttons, battery) also go to
 * hid-generic, so the mouse works without tdxd: tdxd only reads along for the
 * battery level and reports the pairing for the settings panel. */
struct rcv_ctx {
	struct tdx_hid_node node;
	int fd;
	struct tdx_rcv_slot slot; /* the device on this interface (feature 0x42) */
	int battery, charging;    /* battery -1 = not reported yet */
	long long seen_ms;        /* last report from that device, 0 = none */
	long long slot_ms;        /* when the slot was last read */
	uint8_t cfg_sent[TDX_RCV_MOUSE_CONFIG_LEN]; /* CadMouse configuration last sent */
	int cfg_valid;            /* ... and accepted */
	long long cfg_fail_ms;    /* last failed send, to retry no sooner than 10 s */
	struct tdx_uinput ui;     /* keyboard for its keystroke buttons */
	int ui_tried;
	uint8_t buttons;          /* report 0x03 bits last seen */
};
static struct rcv_ctx rcvs[MAX_RCV];
static int nrcv;

/* Add device: one receiver at a time, kept by its USB path, since the
 * receiver drops off USB and comes back once a device has paired. */
enum { PAIR_IDLE, PAIR_ON, PAIR_DONE, PAIR_TIMEOUT };
static struct {
	int state;
	char phys[64];
	int mode;
	long long until_ms, poll_ms;
	struct tdx_rcv_slot before[TDX_RCV_SLOTS];
	int slot;               /* PAIR_DONE: the new device's slot ... */
	struct tdx_rcv_slot got; /* ... and what it is */
	int off_pending;        /* pairing mode still to be turned off */
} pairing;
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
/* Keys an application handles itself while it has the focus, with its names for
 * them (tdxctl app-buttons; the FreeCAD add-on, per workbench): tdxd shows the
 * names on the LCD and leaves the keys to it - no shortcut, no action, only the
 * spacenav button event the application acts on. */
static char appkeys_app[64];
static struct tdx_named_label appkeys[TDX_MAX_NAMED_LABELS];
static int nappkeys;
static int ctl_fd = -1;
static int lcdmgr_fd = -1; /* held "lcd-release" connection from tdx-lcdmgr */
static char ctl_path[108];
static int key_union[3 * TDX_MAX_KEYMAP];
static int nkey_union;

static void on_sig(int s) { if (s == SIGHUP) g_reload = 1; else g_quit = 1; }
static void open_settings(void);
static void open_radial(void);
static int device_rezero(struct dev_ctx *d);

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

/* Feature 0x42: the device this receiver interface carries. 1 = a device,
 * 0 = none, -1 = no answer (not a Universal Receiver style receiver). */
static int rcv_get_slot(int fd, struct tdx_rcv_slot *s)
{
	uint8_t b[8] = {TDX_RCV_REPORT_SELF};
	int r = ioctl(fd, HIDIOCGFEATURE(sizeof b), b);
	if (r < (int)sizeof b) {
		memset(s, 0, sizeof *s);
		return -1;
	}
	return tdx_rcv_parse_slot(b, (size_t)r, s);
}

static const char *rcv_dev_name(const struct rcv_ctx *r)
{
	const struct tdx_device_def *def = tdx_device_lookup(r->node.vid, r->slot.pid);
	return def ? def->name : "Unknown device";
}

static void rcv_update_slot(struct rcv_ctx *r)
{
	struct tdx_rcv_slot s;
	r->slot_ms = now_ms();
	if (rcv_get_slot(r->fd, &s) < 0)
		return; /* keep what we had: the receiver did not answer this time */
	if (s.code == r->slot.code && !strcmp(s.id, r->slot.id))
		return;
	r->slot = s;
	r->battery = -1;
	r->charging = 0;
	r->seen_ms = 0;
	if (s.code)
		LOG(0, "%s: %s paired on slot %d (%s)\n", r->node.def->name, rcv_dev_name(r),
		    r->node.iface, r->node.devnode);
}

static void rcv_close(struct rcv_ctx *r)
{
	LOG(0, "device removed: %s (%s)\n", r->node.def->name, r->node.devnode);
	if (r->fd >= 0)
		close(r->fd);
	tdx_uinput_close(&r->ui);
	*r = rcvs[--nrcv];
}

/* The keyboard device for a CadMouse's keystroke buttons (every key any
 * profile binds, and the whole keyboard), opened as soon as it has such a
 * button, before it is pressed: a desktop may miss the first key of a new
 * device. */
static void rcv_open_keys(struct rcv_ctx *r)
{
	if (r->ui.kfd >= 0 || r->ui_tried || !cfg.enable_uinput)
		return;
	r->ui_tried = 1;
	struct tdx_config *u = malloc(sizeof *u);
	if (!u)
		return;
	*u = cfg;
	u->nkeymap = 0;
	for (int k = 0; k < nkey_union; k += 3) {
		struct tdx_keymap *m = &u->keymap[u->nkeymap++];
		memset(m, 0, sizeof *m);
		for (int j = 0; j < 3 && k + j < nkey_union; j++)
			m->keys[j] = key_union[k + j];
	}
	if (u->nkeymap < TDX_MAX_KEYMAP) /* at least one key, or no device at all */
		u->keymap[u->nkeymap++] = (struct tdx_keymap){.keys = {KEY_ESC}};
	if (tdx_uinput_open_keys(&r->ui, tdx_device_lookup(r->node.vid, r->slot.pid), u))
		LOG(0, "%s: no keyboard device for its buttons\n", rcv_dev_name(r));
	free(u);
}

/* Send a paired CadMouse its configuration (receiver.h) while it is awake:
 * once it is heard from, when the settings change, and after 30 s without a
 * report, in case it was switched off and lost it (3DxWare resends on every
 * change; the mouse cannot be asked what it has). */
static void rcv_mouse_config(struct rcv_ctx *r, long long gap_ms)
{
	if (r->slot.cls != TDX_RCV_2DMOUSE)
		return;
	uint8_t rep[TDX_RCV_MOUSE_CONFIG_LEN], map[7];
	const struct tdx_device_def *def = tdx_device_lookup(r->node.vid, r->slot.pid);
	unsigned reports = tdx_rcv_mouse_map(&cfg, def, map);
	for (int i = 0; i < 7; i++)
		if ((reports & (1u << i)) && tdx_rcv_mouse_binding(&cfg, def, i))
			rcv_open_keys(r); /* a keystroke button */
	tdx_rcv_mouse_config(rep, cfg.mouse_dpi, cfg.mouse_polling_rate, cfg.mouse_smart_scrolling,
			     map);
	if (r->cfg_valid && !memcmp(rep, r->cfg_sent, sizeof rep) && gap_ms < 30000)
		return;
	if (r->cfg_fail_ms && now_ms() - r->cfg_fail_ms < 10000)
		return;
	if (ioctl(r->fd, HIDIOCSFEATURE(sizeof rep), rep) < 0) {
		LOG(0, "%s: cannot send the mouse settings: %s\n", rcv_dev_name(r), strerror(errno));
		r->cfg_valid = 0;
		r->cfg_fail_ms = now_ms();
		return;
	}
	memcpy(r->cfg_sent, rep, sizeof rep);
	r->cfg_valid = 1;
	r->cfg_fail_ms = 0;
	LOG(1, "%s: settings sent: %d dpi, %d Hz, Smart Scrolling %s, buttons %d %d %d %d %d %d %d\n",
	    rcv_dev_name(r), cfg.mouse_dpi, cfg.mouse_polling_rate,
	    cfg.mouse_smart_scrolling ? "on" : "off", map[0], map[1], map[2], map[3], map[4],
	    map[5], map[6]);
}

/* A CadMouse button that reports in 0x03 (its binding is keystrokes):
 * the keys, through a keyboard-only uinput device opened when first needed. */
static void rcv_mouse_buttons(struct rcv_ctx *r, uint8_t bits)
{
	const struct tdx_device_def *def = tdx_device_lookup(r->node.vid, r->slot.pid);
	uint8_t changed = bits ^ r->buttons;
	r->buttons = bits;
	for (int i = 0; i < 7 && changed; i++) {
		if (!(changed & (1u << i)))
			continue;
		int p = (bits >> i) & 1;
		int act = tdx_rcv_mouse_action(&cfg, def, i);
		if (act != TDX_ACT_NONE) { /* a driver action, as on the 3D mice */
			if (!p)
				continue;
			if (act == TDX_ACT_SETTINGS)
				open_settings();
			else if (act == TDX_ACT_COMMONTOOLS)
				open_radial();
			else if (act == TDX_ACT_CALIBRATE)
				for (int k = 0; k < ndev; k++)
					device_rezero(&devs[k]);
			else if (tdx_apply_action(&cfg, act)) {
				LOG(0, "%s: %s: speed %.2f, pan/zoom %s, rotation %s, dominant %s\n",
				    rcv_dev_name(r), tdx_action_name(act), cfg.sensitivity,
				    cfg.disable_translation ? "off" : "on",
				    cfg.disable_rotation ? "off" : "on", cfg.dominant ? "on" : "off");
				if (act == TDX_ACT_SPEED_UP || act == TDX_ACT_SPEED_DOWN)
					info_slider = (int)(cfg.sensitivity * 100 + 0.5);
				info_until = now_ms() + 3000;
				lcdcfg_dirty = 1;
			}
			continue;
		}
		const struct tdx_keymap *km = tdx_rcv_mouse_binding(&cfg, def, i);
		LOG(2, "%s: button %d (%s) %s\n", rcv_dev_name(r), i + 1,
		    def && i < def->nbuttons ? def->buttons[i].v3dk : "-", p ? "down" : "up");
		if (!km && def && i < def->nbuttons &&
		    !strcmp(def->buttons[i].v3dk, "V2DK_RADIALMENU")) {
			if (p) /* its default, as 3DxWare: the Common Tools radial menu */
				open_radial();
			continue;
		}
		if (!km || (km->keys[0] >= BTN_MISC && km->keys[0] < KEY_OK))
			continue; /* nothing, or a mouse function */
		rcv_open_keys(r);
		tdx_uinput_keys(&r->ui, km, p);
	}
}

/* The receiver's control interface (the lowest interface of @phys; any
 * receiver for NULL), or NULL while it is away. */
static struct rcv_ctx *rcv_control(const char *phys)
{
	struct rcv_ctx *best = NULL;
	for (int i = 0; i < nrcv; i++)
		if ((!phys || !strcmp(rcvs[i].node.phys, phys)) &&
		    (!best || rcvs[i].node.iface < best->node.iface))
			best = &rcvs[i];
	return best;
}

static int rcv_slot_table(struct rcv_ctx *r, struct tdx_rcv_slot s[TDX_RCV_SLOTS])
{
	for (int i = 0; i < TDX_RCV_SLOTS; i++) {
		uint8_t b[8] = {(uint8_t)(TDX_RCV_REPORT_SLOT0 + i)};
		if (ioctl(r->fd, HIDIOCGFEATURE(sizeof b), b) < (int)sizeof b ||
		    tdx_rcv_parse_slot(b, sizeof b, &s[i]) < 0)
			return -1;
	}
	return 0;
}

static int rcv_command(struct rcv_ctx *r, const uint8_t cmd[5])
{
	uint8_t b[5];
	memcpy(b, cmd, sizeof b);
	return ioctl(r->fd, HIDIOCSFEATURE(sizeof b), b) < 0 ? -errno : 0;
}

static void pairing_off(void)
{
	struct rcv_ctx *r = rcv_control(pairing.phys);
	uint8_t cmd[5];
	tdx_rcv_cmd_pairing(cmd, TDX_RCV_PAIR_OFF);
	pairing.off_pending = !r || rcv_command(r, cmd) < 0; /* away: when it is back */
}

/* Once a second while pairing: a slot that changed to a device is the new
 * pairing; after 55 s, give up (3DxPair). */
static void pairing_tick(void)
{
	if (pairing.state == PAIR_IDLE)
		return;
	if (pairing.off_pending && rcv_control(pairing.phys))
		pairing_off();
	if (pairing.state != PAIR_ON || now_ms() - pairing.poll_ms < 1000)
		return;
	pairing.poll_ms = now_ms();
	struct rcv_ctx *r = rcv_control(pairing.phys);
	struct tdx_rcv_slot s[TDX_RCV_SLOTS];
	if (r && rcv_slot_table(r, s) == 0)
		for (int i = 0; i < TDX_RCV_SLOTS; i++)
			if (s[i].code && (s[i].code != pairing.before[i].code ||
					  strcmp(s[i].id, pairing.before[i].id))) {
				pairing.state = PAIR_DONE;
				pairing.slot = i;
				pairing.got = s[i];
				const struct tdx_device_def *d = tdx_device_lookup(r->node.vid, s[i].pid);
				LOG(0, "%s: %s paired on slot %d\n", r->node.def->name,
				    d ? d->name : "a device", i);
				pairing_off();
				return;
			}
	if (now_ms() > pairing.until_ms) {
		pairing.state = PAIR_TIMEOUT;
		LOG(0, "pairing: no new device in %d s\n", TDX_RCV_PAIRING_SECONDS);
		pairing_off();
	}
}

static void rcv_read(struct rcv_ctx *r)
{
	uint8_t buf[64];
	for (;;) {
		ssize_t n = read(r->fd, buf, sizeof buf);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return;
			rcv_close(r);
			return;
		}
		if (n == 0)
			return;
		if (!r->slot.code) /* reports from an empty slot: something was just paired */
			rcv_update_slot(r);
		long long gap = r->seen_ms ? now_ms() - r->seen_ms : 1LL << 40;
		r->seen_ms = now_ms();
		rcv_mouse_config(r, gap);
		if (buf[0] == 0x03 && n >= 2 && r->slot.cls == TDX_RCV_2DMOUSE)
			rcv_mouse_buttons(r, buf[1]);
		int lvl, chg;
		if (tdx_rcv_parse_battery(buf, (size_t)n, &lvl, &chg)) {
			if (lvl != r->battery || chg != r->charging)
				LOG(1, "%s: battery %d%%%s\n", rcv_dev_name(r), lvl,
				    chg ? " (charging)" : "");
			r->battery = lvl;
			r->charging = chg;
		}
	}
}

/* `tdxctl status` lines for the receivers: one per receiver, then one per
 * paired device (slot, name, ids, battery, seconds since its last report).
 * "active": the device has sent something since the receiver was plugged in.
 * The receiver keeps its pairing table whether the device is on or off and
 * does not report a connection, so like 3DxWare (HIDDev's ActiveState, set by
 * the first input report and never timed out) that is the only sign of it. */
static size_t rcv_status(char *reply, size_t len, size_t o)
{
	for (int i = 0; i < nrcv && o < len; i++) {
		int first = 1;
		for (int j = 0; j < i; j++)
			if (!strcmp(rcvs[j].node.phys, rcvs[i].node.phys))
				first = 0;
		if (!first)
			continue;
		const struct rcv_ctx *r = &rcvs[i];
		o += (size_t)snprintf(reply + o, len - o, "receiver %s [%04x:%04x] firmware %x.%02x\n",
				      r->node.def->name, r->node.vid, r->node.pid,
				      r->node.version >> 8, r->node.version & 0xFF);
		if (pairing.state != PAIR_IDLE && !strcmp(pairing.phys, r->node.phys)) {
			const struct tdx_device_def *d =
				tdx_device_lookup(r->node.vid, pairing.got.pid);
			if (pairing.state == PAIR_ON)
				o += (size_t)snprintf(reply + o, len - o, "pairing on %lld\n",
						      (pairing.until_ms - now_ms() + 999) / 1000);
			else if (pairing.state == PAIR_DONE)
				o += (size_t)snprintf(reply + o, len - o, "pairing done %d %s\n",
						      pairing.slot, d ? d->name : "Unknown device");
			else
				o += (size_t)snprintf(reply + o, len - o, "pairing timeout\n");
		}
		for (int j = 0; j < nrcv && o < len; j++) {
			const struct rcv_ctx *p = &rcvs[j];
			if (strcmp(p->node.phys, r->node.phys) || !p->slot.code)
				continue;
			char bat[12] = "-", seen[24] = "-";
			if (p->battery >= 0)
				snprintf(bat, sizeof bat, "%d", p->battery);
			if (p->seen_ms)
				snprintf(seen, sizeof seen, "%lld", (now_ms() - p->seen_ms) / 1000);
			o += (size_t)snprintf(reply + o, len - o,
					      "paired %d %s [%04x:%04x] id %s battery %s charging %s seen %s "
					      "active %s serial %s\n",
					      p->node.iface, rcv_dev_name(p), p->node.vid, p->slot.pid,
					      p->slot.id, bat, p->charging ? "yes" : "no", seen,
					      p->seen_ms ? "yes" : "no", p->slot.serial);
		}
	}
	return o;
}

/* ---- the settings panel (3DxWare's Driver_ShowDriverGUI on MENU) ----
 * tdxd runs as root outside any desktop session, so it asks the session of
 * the user at the screen (logind's active session on seat0) to start
 * tdx-settings, through that user's own systemd manager: the panel gets the
 * session's display. A fixed unit name means a second press while the panel is
 * open starts no second one. Double fork: tdxd never waits or leaves zombies. */
#define SESSION_USER \
	"s=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null)\n" \
	"[ -n \"$s\" ] || exit 0\n" \
	"u=$(loginctl show-session \"$s\" -p Name --value 2>/dev/null)\n" \
	"[ -n \"$u\" ] && [ \"$u\" != root ] || exit 0\n"

static const char settings_script[] =
	SESSION_USER
	"exec systemd-run --user --machine=\"$u@\" --unit=tdx-settings-menu --collect --quiet "
	"tdx-settings\n";

/* 3DxWare's radial menu (3DxPieMenus.exe): tdx-radial at the pointer. A press
 * while it is open closes it. Started from a desktop's own terminal (tdxd
 * with a DISPLAY, as when testing), it runs there; tdx-radial itself closes
 * an open one then. */
static const char radial_script[] =
	"s=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null)\n"
	"if [ -z \"$s\" ]; then\n"
	"  [ -n \"$DISPLAY$WAYLAND_DISPLAY\" ] && exec tdx-radial -s \"$TDX_SOCK\"\n"
	"  exit 0\n"
	"fi\n"
	"u=$(loginctl show-session \"$s\" -p Name --value 2>/dev/null)\n"
	"[ -n \"$u\" ] && [ \"$u\" != root ] || exit 0\n"
	"if systemctl --user --machine=\"$u@\" is-active --quiet tdx-radial-menu; then\n"
	"  exec systemctl --user --machine=\"$u@\" stop tdx-radial-menu\n"
	"fi\n"
	"exec systemd-run --user --machine=\"$u@\" --unit=tdx-radial-menu --collect --quiet "
	"tdx-radial -s \"$TDX_SOCK\"\n";

static void session_run(const char *script)
{
	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		setsid();
		if (fork() == 0) {
			int nul = open("/dev/null", O_RDWR);
			if (nul >= 0) {
				dup2(nul, 0);
				dup2(nul, 1);
				if (!cli_verbose)
					dup2(nul, 2);
			}
			setenv("TDX_SOCK", cfg.control_socket, 1); /* tdx-radial's Undo/Redo */
			execl("/bin/sh", "sh", "-c", script, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}

static void open_radial(void)
{
	session_run(radial_script);
	LOG(1, "radial menu: asked the active session to show Common Tools\n");
}

static void open_settings(void)
{
	session_run(settings_script);
	LOG(1, "MENU: asked the active session to open tdx-settings\n");
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

/* The desktop user (whoever runs tdx-focus): their own imported 3DxWare
 * profiles are read too. -1: none yet. */
static int session_uid = -1;
static int nown_profiles; /* how many of them are profile_dir's */

/* Base config, profile list, and the union of all keys any profile can send. */
static void load_all(void)
{
	tdx_config_compose(&cfg, conf_path, &profiles, -1); /* profile_dir lives in the base */
	int rc = tdx_profiles_scan(cfg.profile_dir, &profiles);
	if (rc)
		fprintf(stderr, "tdxd: cannot read %s: %s\n", cfg.profile_dir, strerror(-rc));
	nown_profiles = profiles.n;
	/* then the profiles imported from 3DxWare's application configurations
	 * (tdx-extract-3dxware-ui); the first folder with a match wins */
	static const char *const imported[] = {"/usr/local/share/tdxd/3dxware-ui/profiles",
					       "/usr/share/tdxd/3dxware-ui/profiles"};
	for (size_t i = 0; i < sizeof imported / sizeof imported[0]; i++)
		tdx_profiles_add(imported[i], &profiles, 0);
	struct passwd *pw = session_uid > 0 ? getpwuid((uid_t)session_uid) : NULL;
	if (pw && pw->pw_dir) {
		char dir[512];
		snprintf(dir, sizeof dir, "%s/.local/share/tdxd/3dxware-ui/profiles", pw->pw_dir);
		tdx_profiles_add(dir, &profiles, 1);
	}
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
static int appkeys_active(void)
{
	return nappkeys && focus_known && !strcasecmp(focus_app, appkeys_app);
}

/* cfg is recomposed on every profile switch: put the application's names back */
static void appkeys_to_cfg(void)
{
	cfg.napp_label = appkeys_active() ? nappkeys : 0;
	memcpy(cfg.app_label, appkeys, sizeof appkeys);
}

static int appkeys_claims(const char *v3dk)
{
	if (!v3dk || !appkeys_active())
		return 0;
	for (int i = 0; i < nappkeys; i++)
		if (!strcmp(appkeys[i].v3dk, v3dk))
			return 1;
	return 0;
}

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
	appkeys_to_cfg();
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
	/* a CadMouse's settings and buttons (the profile's): now, if it is awake;
	 * else when it is next heard from */
	for (int i = 0; i < nrcv; i++)
		if (rcvs[i].seen_ms && now_ms() - rcvs[i].seen_ms < 5000)
			rcv_mouse_config(&rcvs[i], 0);
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

/* A keyboard device for keys that come from no button (the radial menu's):
 * a CadMouse's, else a 3D mouse's. */
static struct tdx_uinput *keys_device(void)
{
	for (int i = 0; i < nrcv; i++) {
		if (rcvs[i].slot.cls != TDX_RCV_2DMOUSE)
			continue;
		rcv_open_keys(&rcvs[i]);
		if (rcvs[i].ui.kfd >= 0)
			return &rcvs[i].ui;
	}
	for (int i = 0; i < ndev; i++)
		if (devs[i].ui.kfd >= 0)
			return &devs[i].ui;
	return NULL;
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
		int fpid = pid ? atoi(pid) : 0;
		char resolved[64];
		/* the helpers report /proc/<pid>/exe: for a script that is its
		 * interpreter (python3.14), so name it by the script instead */
		if (fpid > 0 && tdx_app_from_pid(fpid, resolved, sizeof resolved) == 0)
			app = resolved;
		if (tdx_is_own_app(app)) { /* our own panel: keep the application */
			snprintf(reply, len, "ok ignored (%s is tdxd's own) profile=%s\n", app,
				 profile_name(active_profile));
			return;
		}
		int uid = tdx_ctl_peer_uid();
		if (uid > 0 && uid != session_uid) { /* a (new) desktop user: their profiles */
			session_uid = uid;
			load_all();
		}
		snprintf(focus_app, sizeof focus_app, "%s", app);
		focus_pid = fpid;
		focus_known = 1;
		select_profile();
		lcdcfg_write(); /* the application name changed even if the profile didn't */
		snprintf(reply, len, "ok profile=%s\n", profile_name(active_profile));
	} else if (!strcmp(verb, "pair")) {
		/* Add device: "pair" (a CadMouse) or "pair spacemouse" */
		char *kind = strtok_r(NULL, " \t", &save);
		int mode = kind && !strncasecmp(kind, "spacemouse", 10) /* and spacemousepro */
				   ? TDX_RCV_PAIR_ROUND_ROBIN : TDX_RCV_PAIR_EUCLID;
		struct rcv_ctx *r = rcv_control(NULL);
		struct tdx_rcv_slot s[TDX_RCV_SLOTS];
		if (!r) {
			snprintf(reply, len, "error: no Universal Receiver\n");
			return;
		}
		if (rcv_slot_table(r, s) < 0) {
			snprintf(reply, len, "error: cannot read the receiver's pairings\n");
			return;
		}
		int free_slots = 0;
		for (int i = 0; i < TDX_RCV_SLOTS; i++)
			free_slots += !s[i].code;
		if (!free_slots) {
			snprintf(reply, len, "error: %d devices are paired already; remove one\n",
				 TDX_RCV_SLOTS);
			return;
		}
		uint8_t cmd[5];
		tdx_rcv_cmd_pairing(cmd, mode);
		int rc = rcv_command(r, cmd);
		if (rc < 0) {
			snprintf(reply, len, "error: cannot start pairing: %s\n", strerror(-rc));
			return;
		}
		memset(&pairing, 0, sizeof pairing);
		pairing.state = PAIR_ON;
		pairing.mode = mode;
		snprintf(pairing.phys, sizeof pairing.phys, "%s", r->node.phys);
		memcpy(pairing.before, s, sizeof s);
		pairing.until_ms = now_ms() + TDX_RCV_PAIRING_SECONDS * 1000;
		pairing.poll_ms = now_ms();
		LOG(0, "%s: pairing on (mode %d) for %d s\n", r->node.def->name, mode,
		    TDX_RCV_PAIRING_SECONDS);
		snprintf(reply, len, "ok pairing for %d s\n", TDX_RCV_PAIRING_SECONDS);
	} else if (!strcmp(verb, "pair-stop")) {
		if (pairing.state == PAIR_ON)
			pairing_off();
		pairing.state = PAIR_IDLE;
		snprintf(reply, len, "ok\n");
	} else if (!strcmp(verb, "unpair")) {
		char *a = strtok_r(NULL, " \t", &save);
		int slot = a ? atoi(a) : -1;
		struct rcv_ctx *r = rcv_control(NULL);
		uint8_t cmd[5];
		if (!a || slot < 0 || slot >= TDX_RCV_SLOTS) {
			snprintf(reply, len, "error: unpair needs a slot (0-%d)\n", TDX_RCV_SLOTS - 1);
			return;
		}
		if (!r) {
			snprintf(reply, len, "error: no Universal Receiver\n");
			return;
		}
		tdx_rcv_cmd_unpair(cmd, slot);
		int rc = rcv_command(r, cmd);
		if (rc < 0)
			snprintf(reply, len, "error: %s\n", strerror(-rc));
		else {
			LOG(0, "%s: slot %d freed\n", r->node.def->name, slot);
			snprintf(reply, len, "ok slot %d freed\n", slot);
		}
	} else if (!strcmp(verb, "keys")) {
		/* the radial menu's Undo/Redo: "keys KEY_LEFTCTRL+KEY_Z", tapped on a
		 * keyboard device of tdxd's; only root or the desktop user may type */
		int uid = tdx_ctl_peer_uid();
		char *chord = strtok_r(NULL, " \t", &save);
		if (uid != 0 && uid != session_uid) {
			snprintf(reply, len, "error: not allowed\n");
			return;
		}
		struct tdx_keymap km;
		memset(&km, 0, sizeof km);
		km.seq = 1; /* one chord, tapped */
		int n = 0;
		for (char *s2 = NULL, *t = chord ? strtok_r(chord, "+", &s2) : NULL; t;
		     t = strtok_r(NULL, "+", &s2)) {
			int code = tdx_key_from_name(t);
			if (code <= 0 || n >= TDX_MAX_KEYS || (code >= BTN_MISC && code < KEY_OK)) {
				snprintf(reply, len, "error: bad key %s\n", t);
				return;
			}
			km.keys[n++] = code;
		}
		struct tdx_uinput *ui = keys_device();
		if (!n)
			snprintf(reply, len, "error: keys needs KEY_A+KEY_B...\n");
		else if (!ui)
			snprintf(reply, len, "error: no keyboard device\n");
		else {
			tdx_uinput_keys(ui, &km, 1);
			snprintf(reply, len, "ok %d keys\n", n);
		}
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
		/* from the settings panel after an import: its user's profiles too */
		int uid = tdx_ctl_peer_uid();
		if (uid > 0)
			session_uid = uid;
		g_reload = 1;
		snprintf(reply, len, "ok reloading\n");
	} else if (!strcmp(verb, "app-keys")) {
		/* the names an application gave its keys (app-buttons), for the
		 * settings panel: "app NAME" then "V3DK<TAB>name" lines */
		size_t o = (size_t)snprintf(reply, len, "app %s\n", nappkeys ? appkeys_app : "-");
		for (int i = 0; i < nappkeys && o < len; i++)
			o += (size_t)snprintf(reply + o, len - o, "%s\t%s\n", appkeys[i].v3dk,
					      appkeys[i].text);
	} else if (!strcmp(verb, "buttons")) {
		/* each key's number (= the spacenav button number apps get) and V3DK id */
		const struct dev_ctx *d = NULL;
		for (int i = 0; i < ndev && !d; i++)
			if (devs[i].node.def->nbuttons)
				d = &devs[i];
		size_t o = 0;
		for (int i = 0; d && i < d->node.def->nbuttons && o < len; i++)
			if (d->node.def->buttons[i].v3dk)
				o += (size_t)snprintf(reply + o, len - o, "%d %s\n", i,
						      d->node.def->buttons[i].v3dk);
		if (!d)
			snprintf(reply, len, "error: no device\n");
	} else if (!strcmp(verb, "app-buttons")) {
		/* app-buttons PID [TAB V3DK TAB name]...  (fields split by tabs: names
		 * have spaces). The application is named from PID like focus; PID 0 is
		 * the caller (the FreeCAD add-on: inside a Flatpak its own PID means
		 * nothing here). No pairs clears. */
		char *rest = save ? save : (char *)"";
		char *tsave = NULL, *pid = strtok_r(rest, "\t", &tsave);
		int p = pid ? atoi(pid) : -1;
		if (p == 0 && pid && pid[0] == '0')
			p = tdx_ctl_peer_pid();
		char name[64];
		if (p <= 0 || tdx_app_from_pid(p, name, sizeof name)) {
			snprintf(reply, len, "error: app-buttons needs the process id of the application\n");
			return;
		}
		snprintf(appkeys_app, sizeof appkeys_app, "%s", name);
		nappkeys = 0;
		for (char *k; (k = strtok_r(NULL, "\t", &tsave)) && nappkeys < TDX_MAX_NAMED_LABELS;) {
			char *t = strtok_r(NULL, "\t", &tsave);
			if (strncmp(k, "V3DK_", 5))
				continue;
			snprintf(appkeys[nappkeys].v3dk, sizeof appkeys[0].v3dk, "%s", k);
			snprintf(appkeys[nappkeys].text, sizeof appkeys[0].text, "%s", t ? t : "");
			nappkeys++;
		}
		lcdcfg_write();
		snprintf(reply, len, "ok %s: %d keys\n", appkeys_app, nappkeys);
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
		o = rcv_status(reply, len, o);
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
		for (int j = 0; j < nrcv; j++)
			if (!strcmp(rcvs[j].node.devnode, nodes[i].devnode))
				known = 1;
		if (known)
			continue;
		int fd = open(nodes[i].devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			fd = open(nodes[i].devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			LOG(1, "cannot open %s: %s\n", nodes[i].devnode, strerror(errno));
			continue;
		}
		struct tdx_rcv_slot slot;
		if (nodes[i].def->dev_class == TDX_CLASS_RECEIVER && rcv_get_slot(fd, &slot) >= 0) {
			/* A paired 3D mouse whose reports come through this interface is
			 * driven like the wired one (untested: no such device here). */
			const struct tdx_device_def *pd =
				slot.cls == TDX_RCV_3DMOUSE ? tdx_device_lookup(nodes[i].vid, slot.pid) : NULL;
			if (pd && pd->dev_class == TDX_CLASS_3DMOUSE) {
				nodes[i].def = pd;
			} else {
				if (nrcv >= MAX_RCV) {
					close(fd);
					continue;
				}
				struct rcv_ctx *r = &rcvs[nrcv++];
				memset(r, 0, sizeof *r);
				r->node = nodes[i];
				r->fd = fd;
				r->ui.fd = r->ui.kfd = -1;
				r->battery = -1;
				int seen = 0;
				for (int j = 0; j < nrcv - 1; j++)
					seen |= !strcmp(rcvs[j].node.phys, r->node.phys);
				if (!seen) /* once per receiver, not per interface */
					LOG(0, "device: %s [%04x:%04x] on %s\n", r->node.def->name,
					    r->node.vid, r->node.pid, r->node.devnode);
				rcv_update_slot(r);
				continue;
			}
		}
		struct dev_ctx *d = &devs[ndev];
		memset(d, 0, sizeof *d);
		d->node = nodes[i];
		d->ui.fd = d->ui.kfd = -1;
		d->slide_target = -1;
		d->fd = fd;
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
	for (int i = 0; i < nrcv; i++) /* pairing changes (Add device, another receiver) */
		if (now_ms() - rcvs[i].slot_ms > 10000)
			rcv_update_slot(&rcvs[i]);
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
		appkeys_to_cfg();
		if (p && cfg.lcd_cfg[0]) { /* LCDCfg.xml <ButtonPressed>, cleared after 3 s */
			tdx_button_text(&cfg, d->node.def, i, info_pressed, sizeof info_pressed);
			info_until = now_ms() + 3000;
			lcdcfg_dirty = 1;
		}
		if (appkeys_claims(v3dk)) { /* the application runs its own command for it */
			tdx_uinput_button(&d->ui, i, p);
			if (spnav.lfd >= 0)
				tdx_spnav_button(&spnav, i, p);
			continue;
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
			else if (act == TDX_ACT_SETTINGS)
				open_settings();
			else if (act == TDX_ACT_COMMONTOOLS)
				open_radial();
			continue;
		}
		const struct tdx_keymap *km = tdx_keymap_for(&cfg, d->node.def, i);
		if (km)
			tdx_uinput_keys(&d->ui, km, p);
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
	if (nown_profiles)
		LOG(0, "%d per-application profile(s) in %s\n", nown_profiles, cfg.profile_dir);
	if (profiles.n > nown_profiles)
		LOG(0, "%d imported from 3DxWare's application settings\n", profiles.n - nown_profiles);

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
			for (int i = 0; i < nrcv; i++) { /* key bindings may have changed */
				tdx_uinput_close(&rcvs[i].ui);
				rcvs[i].ui_tried = 0;
			}
			refresh_devices();
			bmp_watch_setup();
			bmp_send_pending();
		}
		if (now_ms() - last_scan > 2000) {
			rescan();
			last_scan = now_ms();
		}
		pairing_tick();

		struct pollfd pfd[MAX_DEV + MAX_RCV + 4 + TDX_SPNAV_MAX_CLIENTS];
		int np = 0;
		for (int i = 0; i < ndev; i++)
			pfd[np++] = (struct pollfd){.fd = devs[i].fd, .events = POLLIN};
		int rcv_base = np, nrcv_polled = nrcv;
		for (int i = 0; i < nrcv; i++)
			pfd[np++] = (struct pollfd){.fd = rcvs[i].fd, .events = POLLIN};
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
		for (int i = nrcv_polled - 1; i >= 0; i--) /* before devs: their indices come first */
			if (pfd[rcv_base + i].revents & (POLLIN | POLLHUP | POLLERR))
				rcv_read(&rcvs[i]);
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
	while (nrcv)
		rcv_close(&rcvs[nrcv - 1]);
	tdx_spnav_close(&spnav);
	if (ctl_fd >= 0) {
		close(ctl_fd);
		unlink(ctl_path);
	}
	return 0;
}
