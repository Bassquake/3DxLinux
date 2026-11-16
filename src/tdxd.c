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
#include <linux/hidraw.h>

#include "config.h"
#include "gfx.h"
#include "hidraw.h"
#include "lcd.h"
#include "lcd_ent.h"
#include "lcd_giotto.h"
#include "output.h"
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
};

static volatile sig_atomic_t g_quit, g_reload;
static struct tdx_config cfg;
static const char *conf_path = DEFAULT_CONF;
static struct dev_ctx devs[MAX_DEV];
static int ndev;
static struct tdx_spnav spnav = {.lfd = -1};

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
	if (cfg.lcd_brightness >= 0)
		tdx_lcd_set_brightness(d->lcd, cfg.lcd_brightness);
	lcd_redraw(d);
}

/* Enterprise LCD brightness: feature report {0x11, percent} (HIDDev.dll case 8). */
static void lcd_intensity_apply(struct dev_ctx *d)
{
	uint8_t rep[2];
	if (!tdx_lcd_intensity_report(d->node.def, cfg.lcd_brightness, rep))
		return;
	if (ioctl(d->fd, HIDIOCSFEATURE(2), rep) < 0)
		LOG(1, "LCD intensity write failed on %s: %s\n", d->node.devnode, strerror(errno));
	else
		LOG(2, "LCD intensity %d%% on %s\n", rep[1], d->node.devnode);
}

static void led_apply(struct dev_ctx *d)
{
	if (tdx_has_rgb_leds(d->node.def)) {
		uint8_t reps[TDX_RGB_RING_FIRST + TDX_RGB_NUM_RING + 2][8];
		int n = tdx_rgb_plan(&cfg, d->node.def, reps, (int)(sizeof reps / sizeof reps[0]));
		for (int i = 0; i < n; i++)
			if (ioctl(d->fd, HIDIOCSFEATURE(8), reps[i]) < 0) {
				LOG(1, "LED colour write failed on %s: %s\n", d->node.devnode,
				    strerror(errno));
				break;
			}
		LOG(2, "sent %d LED colour report(s)\n", n);
		return;
	}
	uint8_t rep[2];
	if (cfg.led < 0 || !tdx_led_report(d->node.def, cfg.led, rep))
		return;
	if (write(d->fd, rep, sizeof rep) != (ssize_t)sizeof rep)
		LOG(1, "LED write failed on %s: %s\n", d->node.devnode, strerror(errno));
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
		if (cfg.enable_uinput) {
			int rc = tdx_uinput_open(&d->ui, d->node.def, &cfg);
			if (rc)
				LOG(0, "uinput disabled: %s\n", strerror(-rc));
		}
		led_apply(d);
		lcd_intensity_apply(d);
		lcd_attach(d);
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
	puts("usage: tdxd [-c config] [-v]... [-l]\n"
	     "  -c FILE  configuration file (default " DEFAULT_CONF ")\n"
	     "  -v       more logging (repeatable)\n"
	     "  -l       list supported devices and exit");
}

int main(int argc, char **argv)
{
	int opt, verbose = 0;
	while ((opt = getopt(argc, argv, "c:vlh")) != -1) {
		switch (opt) {
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

	tdx_config_defaults(&cfg);
	if (tdx_config_load(&cfg, conf_path) && strcmp(conf_path, DEFAULT_CONF))
		fprintf(stderr, "tdxd: cannot read %s: %s\n", conf_path, strerror(errno));
	if (verbose)
		cfg.verbose = verbose;

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

	long long last_scan = 0;
	while (!g_quit) {
		if (g_reload) {
			g_reload = 0;
			tdx_config_defaults(&cfg);
			tdx_config_load(&cfg, conf_path);
			if (verbose)
				cfg.verbose = verbose;
			LOG(0, "configuration reloaded\n");
			for (int i = 0; i < ndev; i++) {
				if (devs[i].lcd && cfg.lcd_brightness >= 0)
					tdx_lcd_set_brightness(devs[i].lcd, cfg.lcd_brightness);
				led_apply(&devs[i]);
				lcd_intensity_apply(&devs[i]);
				lcd_redraw(&devs[i]);
			}
			bmp_watch_setup();
			bmp_send_pending();
		}
		if (now_ms() - last_scan > 2000) {
			rescan();
			last_scan = now_ms();
		}

		struct pollfd pfd[MAX_DEV + 2 + TDX_SPNAV_MAX_CLIENTS];
		int np = 0;
		for (int i = 0; i < ndev; i++)
			pfd[np++] = (struct pollfd){.fd = devs[i].fd, .events = POLLIN};
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
		if (r <= 0)
			continue;
		if (bmp_idx >= 0 && (pfd[bmp_idx].revents & POLLIN))
			bmp_watch_read();
		for (int i = ndev - 1; i >= 0; i--)
			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
				dev_read(&devs[i]);
		if (spnav.lfd >= 0) {
			if (pfd[sp_base].revents & POLLIN)
				tdx_spnav_accept(&spnav);
			for (int i = 0; i < TDX_SPNAV_MAX_CLIENTS; i++)
				if (spnav.cfd[i] >= 0 && (pfd[sp_base + 1 + i].revents & (POLLIN | POLLHUP)))
					tdx_spnav_client_input(&spnav, i);
		}
	}

	while (ndev)
		dev_close(&devs[ndev - 1]);
	tdx_spnav_close(&spnav);
	return 0;
}
