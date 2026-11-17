/*
 * tdx-lcdmgr - LCD Manager for the SpacePilot PRO's 320x240 LCD (Linux port of
 * Logitech LCDMon.exe 3.06 as shipped with 3DxWare 10).
 *
 * Runs in the user's session (systemd --user). Takes the LCD from tdxd
 * ("lcd-release" on tdxd's control socket, held for as long as we run), serves
 * LCD applets over a Unix socket (lcdmgr_proto.h; libtdxlcd / tdxlcd.py on the
 * applet side), decides which applet is shown (lcdmgr_core.c), runs the
 * on-LCD program list driven by the LCD's soft buttons, starts autostart
 * applets, and keeps LCDMon's settings database (lcdmgr_db.c).
 */
#define _GNU_SOURCE
#include "control.h"
#include "gfx.h"
#include "lcd.h"
#include "lcdmgr_core.h"
#include "lcdmgr_db.h"
#include "lcdmgr_proto.h"
#include "usbfs.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef TDX_VERSION
#define TDX_VERSION "unknown"
#endif

#define MAX_CLIENTS 32
#define SPP_VID 0x046D
#define SPP_PID 0xC629
#define NPIX (TDX_LCD_W * TDX_LCD_H)
#define FRAME_MIN_MS 33
#define IDENTIFY_MS 3000

static int verbose;
#define LOG(lvl, ...) do { if (verbose >= (lvl)) fprintf(stderr, "tdx-lcdmgr: " __VA_ARGS__); } while (0)

struct client {
	int fd;
	int connected;
	uint32_t caps, flags, families;
	char name[LM_NAME_MAX];
	char path[LM_PATH_MAX];
	int pid;
	int prog;
	int dev_open;
	uint32_t *frame;
	int has_frame, priority, fg;
	uint64_t fg_time, alert_time;
	uint32_t buttons;
	struct gfx_img icon; /* LM_ICON (LCDMon took it from the applet's .exe) */
	/* receive state */
	struct lm_hdr h;
	uint32_t got;      /* bytes of header+payload received */
	uint8_t *rx;
	int wait_sync;     /* reply to a synchronous update after the frame is sent */
};

static struct client cl[MAX_CLIENTS];
static int listen_fd = -1;
static char sock_path[108], db_path[LM_PATH_MAX], tdxd_sock[108] = "/run/tdxd.sock";
static char fake_path[LM_PATH_MAX];
static int no_seed; /* -F: virtual LCD written to a PPM file (tests, preview) */
static struct lm_db db;
static struct tdx_lcd lcd = {.fd = -1};
static int lcd_ok, hold_fd = -1;
static uint64_t last_try, last_push, identify_until;
static int dirty, shown_client = -1;
static struct lm_sched sched = {.shown = -1};
static struct lm_menu menu;
static uint32_t fbpx[NPIX];
static volatile sig_atomic_t g_quit;
static uint64_t spawned_at[LM_MAX_PROG];
static pid_t spawned_pid[LM_MAX_PROG]; /* applets we started: stopped when we exit */

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void on_sig(int s) { (void)s; g_quit = 1; }

/* ------------------------------------------------------------------ io */

static int send_msg(struct client *c, uint32_t type, const void *a, uint32_t na, const void *b,
		    uint32_t nb)
{
	struct lm_hdr h = {type, na + nb};
	uint8_t buf[sizeof h + 4096];
	if (sizeof h + na + nb > sizeof buf)
		return -1;
	memcpy(buf, &h, sizeof h);
	if (na)
		memcpy(buf + sizeof h, a, na);
	if (nb)
		memcpy(buf + sizeof h + na, b, nb);
	size_t len = sizeof h + na + nb, off = 0;
	while (off < len) { /* small messages: a blocking write of a few bytes */
		ssize_t w = send(c->fd, buf + off, len - off, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && errno == EAGAIN) {
			struct pollfd p = {.fd = c->fd, .events = POLLOUT};
			if (poll(&p, 1, 200) <= 0)
				return -1;
			continue;
		}
		if (w <= 0)
			return -1;
		off += (size_t)w;
	}
	return 0;
}

static void reply(struct client *c, uint32_t req, uint32_t status, const void *data, uint32_t n)
{
	uint32_t r[2] = {req, status};
	send_msg(c, LM_REPLY, r, sizeof r, data, n);
}

static void notify(struct client *c, uint32_t code, uint32_t p1)
{
	if (!c->connected || !(c->flags & LM_CF_NOTIFY))
		return;
	uint32_t n[5] = {code, p1, 0, 0, 0};
	send_msg(c, LM_EV_NOTIFY, n, sizeof n, NULL, 0);
}

static void notify_all(uint32_t code, uint32_t p1)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (cl[i].fd >= 0)
			notify(&cl[i], code, p1);
}

static void save_db(void)
{
	int rc = lm_db_save(&db, db_path);
	if (rc)
		LOG(0, "cannot save %s: %s\n", db_path, strerror(-rc));
}

/* -------------------------------------------------------- program identity */

static int is_interpreter(const char *base)
{
	static const char *const names[] = {"python", "perl", "ruby", "node", "bash", "sh",
					    "dash", "lua", "tclsh", "php", "java"};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (!strncmp(base, names[i], strlen(names[i])))
			return 1;
	return 0;
}

/* The applet's executable, or its script for interpreted applets. */
static void program_path(int pid, char *out, size_t len)
{
	char p[64], exe[LM_PATH_MAX];
	out[0] = 0;
	snprintf(p, sizeof p, "/proc/%d/exe", pid);
	ssize_t n = readlink(p, exe, sizeof exe - 1);
	if (n <= 0)
		return;
	exe[n] = 0;
	const char *base = strrchr(exe, '/');
	base = base ? base + 1 : exe;
	snprintf(out, len, "%s", exe);
	if (!is_interpreter(base))
		return;
	snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	char cmd[2048];
	ssize_t m = read(fd, cmd, sizeof cmd - 1);
	close(fd);
	if (m <= 0)
		return;
	cmd[m] = 0;
	char cwd[LM_PATH_MAX] = "";
	snprintf(p, sizeof p, "/proc/%d/cwd", pid);
	ssize_t k = readlink(p, cwd, sizeof cwd - 1);
	cwd[k > 0 ? k : 0] = 0;
	for (char *a = cmd + strlen(cmd) + 1; a < cmd + m; a += strlen(a) + 1) {
		if (a[0] == '-') {
			if (!strcmp(a, "-m") || !strcmp(a, "-c"))
				return; /* module / inline code: keep the interpreter */
			continue;
		}
		if (a[0] == '/')
			snprintf(out, len, "%s", a);
		else
			snprintf(out, len, "%s/%s", cwd, a);
		char real[PATH_MAX];
		if (realpath(out, real) && strlen(real) < len)
			memcpy(out, real, strlen(real) + 1);
		return;
	}
}

/* ------------------------------------------------------------ rendering */

static struct tdx_fb fb = {TDX_LCD_W, TDX_LCD_H, fbpx};

/* Candidates: connected programs with the LCD open and enabled. */
static int candidates(struct lm_cand *cand, int *idx)
{
	int n = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &cl[i];
		if (c->fd < 0 || !c->dev_open || c->prog < 0)
			continue;
		const struct lm_program *p = &db.prog[c->prog];
		if (!p->enabled || !p->dev_enabled)
			continue;
		cand[n] = (struct lm_cand){.prog = c->prog, .has_frame = c->has_frame,
					   .priority = c->priority, .foreground = c->fg && p->allow_fg,
					   .fg_time = c->fg_time, .alert_time = c->alert_time};
		idx[n++] = i;
	}
	/* in Programs-list order, like LCDMon's on-LCD list */
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && cand[b - 1].prog > cand[b].prog; b--) {
			struct lm_cand t = cand[b];
			cand[b] = cand[b - 1];
			cand[b - 1] = t;
			int ti = idx[b];
			idx[b] = idx[b - 1];
			idx[b - 1] = ti;
		}
	return n;
}

/* ---- the program list ("Finder"), skinned like LCDMon's
 * BrowserArtPath\FinderConfig.txt from 3Dconnexion's LCD applets package
 * (client_list_header 320x36 "SpacePilot PRO", client_list_background,
 * vscroll_*; Arial 10 in 204,204,204; icon/text offset 36; rows 40 px). */

struct finder {
	int loaded;
	struct gfx_img bg, header, sb_base, sb_top, sb_mid, sb_bottom;
	uint32_t font_rgb;
	int header_text, icon_off, text_off, item_h;
} finder = {.font_rgb = 0xCCCCCC, .icon_off = 36, .text_off = 36, .item_h = 40};
static char assets_dir[LM_PATH_MAX];

static void finder_load(void)
{
	if (finder.loaded)
		return;
	finder.loaded = 1;
	const char *home = getenv("HOME");
	char cands[5][LM_PATH_MAX + 64];
	int n = 0;
	if (assets_dir[0])
		snprintf(cands[n++], sizeof cands[0], "%s", assets_dir);
	if (getenv("TDX_UI_ASSETS"))
		snprintf(cands[n++], sizeof cands[0], "%.255s", getenv("TDX_UI_ASSETS"));
	if (home)
		snprintf(cands[n++], sizeof cands[0], "%.200s/.local/share/tdxd/3dxware-ui", home);
	snprintf(cands[n++], sizeof cands[0], "/usr/local/share/tdxd/3dxware-ui");
	snprintf(cands[n++], sizeof cands[0], "/usr/share/tdxd/3dxware-ui");
	for (int i = 0; i < n; i++) {
		char d[LM_PATH_MAX + 96], p[LM_PATH_MAX + 160];
		snprintf(d, sizeof d, "%.300s/lcdapplets/finder", cands[i]);
		snprintf(p, sizeof p, "%s/client_list_header.pam", d);
		if (access(p, R_OK))
			continue;
		struct { struct gfx_img *img; const char *name; } t[] = {
			{&finder.bg, "client_list_background"}, {&finder.header, "client_list_header"},
			{&finder.sb_base, "vscroll_base"}, {&finder.sb_top, "vscroll_thumb_top"},
			{&finder.sb_mid, "vscroll_thumb_mid"}, {&finder.sb_bottom, "vscroll_thumb_bottom"},
		};
		for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) {
			snprintf(p, sizeof p, "%s/%s.pam", d, t[k].name);
			gfx_load_pam(t[k].img, p);
		}
		snprintf(p, sizeof p, "%s/FinderConfig.txt", d);
		FILE *f = fopen(p, "r");
		char line[256];
		while (f && fgets(line, sizeof line, f)) {
			int a, b, c;
			if (sscanf(line, "client_list_font_RGB=%d %d %d", &a, &b, &c) == 3)
				finder.font_rgb = (uint32_t)a << 16 | (uint32_t)b << 8 | (uint32_t)c;
			sscanf(line, "client_list_icon_offset=%d", &finder.icon_off);
			sscanf(line, "client_list_text_offset=%d", &finder.text_off);
			sscanf(line, "client_list_item_height=%d", &finder.item_h);
			if (!strncmp(line, "client_list_display_header_text=true", 36))
				finder.header_text = 1;
		}
		if (f)
			fclose(f);
		/* LCDMon's clamps (CFinderConfig parser) */
		finder.icon_off = finder.icon_off < 0 ? 0 : finder.icon_off > 120 ? 120 : finder.icon_off;
		finder.text_off = finder.text_off < 7 ? 7 : finder.text_off > 120 ? 120 : finder.text_off;
		finder.item_h = finder.item_h < 36 ? 36 : finder.item_h > 72 ? 72 : finder.item_h;
		LOG(1, "program list skin from %s\n", d);
		return;
	}
}

static void draw_header(const char *title)
{
	finder_load();
	if (finder.bg.rgba)
		gfx_blit(&fb, &finder.bg, 0, 0);
	else
		gfx_vgradient(&fb, 0, 0, TDX_LCD_W, TDX_LCD_H, 0x262626, 0x161616);
	if (finder.header.rgba) {
		gfx_blit(&fb, &finder.header, 0, 0);
		if (finder.header_text && title)
			gfx_text_aa(&fb, 10, 9, title, &gfx_font_f14b, 0xE0E0E0);
	} else {
		gfx_vgradient(&fb, 0, 0, TDX_LCD_W, 36, 0x5A5A5A, 0x0E0E0E);
		gfx_text_aa(&fb, 10, 7, title ? title : "SpacePilot PRO", &gfx_font_f18b, 0xC8C8C8);
	}
}

static void draw_idle(void)
{
	draw_header(NULL);
	const char *a = "No programs running";
	gfx_text_aa(&fb, (TDX_LCD_W - gfx_text_aa_width(a, &gfx_font_f14)) / 2, 128, a,
		    &gfx_font_f14, finder.font_rgb);
}

static void draw_menu(const int *idx, int n)
{
	draw_header(db.family_name);
	int ih = finder.item_h, top_y = 36;
	int rows = (TDX_LCD_H - top_y) / ih;
	int top = menu.sel >= rows ? menu.sel - rows + 1 : 0;
	int sb = n > rows; /* scrollbar only when the list doesn't fit */
	int right = sb ? TDX_LCD_W - 12 : TDX_LCD_W - 20;
	for (int r = 0; r < rows && top + r < n; r++) {
		int i = top + r, y = top_y + r * ih;
		const struct client *c = &cl[idx[i]];
		if (i == menu.sel) { /* highlight bar (colours taken from the device photo) */
			gfx_vgradient(&fb, 0, y + 2, right, ih - 4, 0x707070, 0x4C4C4C);
			gfx_fill(&fb, 0, y + 2, right, 1, 0x8C8C8C);
		}
		int ix = finder.icon_off - 1, iy = y + (ih - 32) / 2;
		if (c->icon.rgba)
			gfx_blit(&fb, &c->icon, ix + (32 - c->icon.w) / 2, iy + (32 - c->icon.h) / 2);
		else { /* no icon from the applet: a neutral tile */
			gfx_vgradient(&fb, ix + 2, iy + 2, 28, 28, 0x5A5A5A, 0x3A3A3A);
		}
		const struct gfx_font *f = &gfx_font_f14;
		char name[LM_NAME_MAX];
		snprintf(name, sizeof name, "%s", c->name);
		int tx = ix + 32 + finder.text_off - 6, maxw = right - tx - 4;
		while (gfx_text_aa_width(name, f) > maxw && strlen(name) > 1)
			name[strlen(name) - 1] = 0;
		gfx_text_aa(&fb, tx, y + (ih - f->height) / 2, name, f,
			    i == menu.sel ? 0xF0F0F0 : finder.font_rgb);
	}
	if (sb) { /* vscroll_base + thumb */
		int sx = TDX_LCD_W - 10, sy = top_y + 8, sh = TDX_LCD_H - top_y - 16;
		if (finder.sb_base.rgba)
			gfx_blit_part(&fb, &finder.sb_base, 0, 0, finder.sb_base.w, sh, sx, sy);
		else
			gfx_fill(&fb, sx + 4, sy, 1, sh, 0x505050);
		int th = sh * rows / n, ty = sy + (sh - th) * top / (n - rows > 0 ? n - rows : 1);
		if (finder.sb_mid.rgba) {
			gfx_blit(&fb, &finder.sb_top, sx + 1, ty);
			for (int yy = ty + finder.sb_top.h; yy < ty + th - finder.sb_bottom.h; yy += finder.sb_mid.h)
				gfx_blit(&fb, &finder.sb_mid, sx + 1, yy);
			gfx_blit(&fb, &finder.sb_bottom, sx + 1, ty + th - finder.sb_bottom.h);
		} else {
			gfx_fill(&fb, sx + 2, ty, 5, th, 0x2A8FD6);
		}
	}
}

static void draw_identify(void)
{
	draw_header(NULL);
	const char *a = "3Dconnexion", *b = db.family_name;
	gfx_text_aa(&fb, (TDX_LCD_W - gfx_text_aa_width(a, &gfx_font_f18b)) / 2, 96, a,
		    &gfx_font_f18b, 0xF0F0F0);
	gfx_text_aa(&fb, (TDX_LCD_W - gfx_text_aa_width(b, &gfx_font_f14)) / 2, 128, b,
		    &gfx_font_f14, finder.font_rgb);
}

static void blit_client(const struct client *c)
{
	memcpy(fbpx, c->frame, sizeof fbpx);
}

static void render(void)
{
	if (!lcd_ok)
		return;
	uint64_t now = now_ms();
	struct lm_cand cand[MAX_CLIENTS];
	int idx[MAX_CLIENTS];
	int n = candidates(cand, idx);
	int manual = lm_db_find(&db, db.manual_path, db.manual_name);
	int k = lm_pick(&sched, cand, n, db.algorithm, db.delay, db.alerts, manual, now);
	int was = shown_client;
	shown_client = k >= 0 ? idx[k] : -1;
	if (was != shown_client) {
		LOG(1, "showing %s\n", shown_client >= 0 ? cl[shown_client].name : "(idle)");
		dirty = 1;
	}
	if (!dirty || now - last_push < FRAME_MIN_MS)
		return;
	if (identify_until && now < identify_until)
		draw_identify();
	else if (menu.open)
		draw_menu(idx, n);
	else if (shown_client >= 0)
		blit_client(&cl[shown_client]);
	else
		draw_idle();
	int rc = 0;
	if (fake_path[0]) {
		char tmp[LM_PATH_MAX + 8];
		snprintf(tmp, sizeof tmp, "%s.tmp", fake_path);
		FILE *f = fopen(tmp, "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", TDX_LCD_W, TDX_LCD_H);
			for (int i = 0; i < NPIX; i++) {
				uint8_t p3[3] = {(uint8_t)(fbpx[i] >> 16), (uint8_t)(fbpx[i] >> 8),
						 (uint8_t)fbpx[i]};
				fwrite(p3, 1, 3, f);
			}
			fclose(f);
			rename(tmp, fake_path);
		}
	} else {
		rc = tdx_lcd_show(&lcd, fbpx);
	}
	last_push = now;
	dirty = 0;
	if (rc == -ENODEV) {
		LOG(0, "LCD gone\n");
		tdx_lcd_close(&lcd);
		lcd_ok = 0;
		notify_all(LGLCD_NOTIFICATION_DEVICE_REMOVAL_, LGLCD_DEVICE_QVGA_);
	}
	/* answer synchronous updates now that a frame went out */
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (cl[i].fd >= 0 && cl[i].wait_sync) {
			cl[i].wait_sync = 0;
			reply(&cl[i], LM_UPDATE, LM_OK, NULL, 0);
		}
}

/* ---------------------------------------------------------- the LCD itself */

static void hold_tdxd(void)
{
	if (hold_fd >= 0)
		return;
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	snprintf(a.sun_path, sizeof a.sun_path, "%s", tdxd_sock);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		close(fd); /* tdxd not running: nothing to take over */
		return;
	}
	const char *cmd = "lcd-release\n";
	char rep[128] = "";
	if (write(fd, cmd, strlen(cmd)) > 0) {
		struct pollfd p = {.fd = fd, .events = POLLIN};
		if (poll(&p, 1, 2000) > 0) {
			ssize_t r = read(fd, rep, sizeof rep - 1);
			rep[r > 0 ? r : 0] = 0;
		}
	}
	if (strncmp(rep, "ok", 2)) {
		LOG(0, "tdxd did not release the LCD: %s\n", rep[0] ? rep : "no answer");
		close(fd);
		return;
	}
	hold_fd = fd;
	LOG(1, "tdxd released the LCD\n");
}

static void lcd_try(void)
{
	if (lcd_ok)
		return;
	uint64_t now = now_ms();
	if (now - last_try < 2000)
		return;
	last_try = now;
	if (fake_path[0]) {
		lcd_ok = 1;
		dirty = 1;
		LOG(0, "virtual LCD -> %s\n", fake_path);
		notify_all(LGLCD_NOTIFICATION_DEVICE_ARRIVAL_, LGLCD_DEVICE_QVGA_);
		return;
	}
	char node[64];
	if (tdx_usb_find_node(SPP_VID, SPP_PID, node, sizeof node))
		return; /* no SpacePilot PRO */
	hold_tdxd();
	int rc = -EBUSY;
	for (int i = 0; i < 10 && rc; i++) {
		rc = tdx_lcd_open(&lcd, SPP_VID, SPP_PID);
		if (rc == -EBUSY)
			usleep(100 * 1000);
		else
			break;
	}
	if (rc) {
		LOG(0, "LCD unavailable: %s\n", strerror(-rc));
		return;
	}
	if (tdx_lcd_wait_ready(&lcd, 20))
		LOG(0, "LCD did not answer the ping; continuing\n");
	tdx_lcd_set_brightness(&lcd, db.brightness);
	if ((rc = tdx_lcd_buttons_start(&lcd)))
		LOG(0, "soft buttons unavailable: %s\n", strerror(-rc));
	lcd_ok = 1;
	dirty = 1;
	LOG(0, "LCD on %s (soft buttons ep 0x%02x)\n", lcd.path, lcd.ep_in);
	notify_all(LGLCD_NOTIFICATION_DEVICE_ARRIVAL_, LGLCD_DEVICE_QVGA_);
}

static void menu_items(int *idx, int *n, int *current)
{
	struct lm_cand cand[MAX_CLIENTS];
	*n = candidates(cand, idx);
	*current = -1;
	for (int i = 0; i < *n; i++)
		if (idx[i] == shown_client)
			*current = i;
}

static void select_program(int prog)
{
	snprintf(db.manual_path, sizeof db.manual_path, "%s", db.prog[prog].path);
	snprintf(db.manual_name, sizeof db.manual_name, "%s", db.prog[prog].name);
	for (int i = 0; i < MAX_CLIENTS; i++) /* an explicit choice beats older requests */
		cl[i].fg = 0;
	struct lm_cand cand[MAX_CLIENTS];
	int idx[MAX_CLIENTS], n = candidates(cand, idx);
	for (int i = 0; i < n; i++)
		if (cand[i].prog == prog) {
			sched.rot = i;
			sched.rot_since = now_ms();
		}
	save_db();
	dirty = 1;
}

static void soft_buttons(uint32_t b)
{
	int idx[MAX_CLIENTS], n, cur, commit = -1;
	menu_items(idx, &n, &cur);
	int r = lm_menu_buttons(&menu, b, n, cur, db.quickswitch, now_ms(), &commit);
	LOG(2, "soft buttons 0x%08x -> %d\n", b, r);
	if (r == LM_MENU_REDRAW)
		dirty = 1;
	else if (r == LM_MENU_COMMIT && commit >= 0 && commit < n) {
		select_program(cl[idx[commit]].prog);
		LOG(0, "switched to %s\n", cl[idx[commit]].name);
	} else if (r == LM_MENU_FORWARD && shown_client >= 0) {
		struct client *c = &cl[shown_client];
		uint32_t mask = b & 0x0000FFFFu; /* lglcd buttons only */
		if (mask != c->buttons || !mask) {
			c->buttons = mask;
			uint32_t ev[2] = {1, mask};
			send_msg(c, LM_EV_BUTTONS, ev, sizeof ev, NULL, 0);
		}
	}
}

/* -------------------------------------------------------- applet requests */

static void client_close(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	free(c->frame);
	free(c->rx);
	gfx_img_free(&c->icon);
	int was_shown = c - cl == shown_client;
	memset(c, 0, sizeof *c);
	c->fd = -1;
	c->prog = -1;
	if (was_shown)
		dirty = 1;
}

static void handle_admin(struct client *c, char *cmd);

static void handle(struct client *c)
{
	uint32_t *u = (uint32_t *)c->rx;
	uint32_t len = c->h.len;
	switch (c->h.type) {
	case LM_CONNECT: {
		if (len < 12 || c->connected) {
			reply(c, LM_CONNECT, LM_ERROR_INVALID_PARAMETER, NULL, 0);
			return;
		}
		if (u[0] != LM_VERSION) {
			reply(c, LM_CONNECT, LM_ERROR_OLD_WIN_VERSION, NULL, 0);
			return;
		}
		c->caps = u[1];
		c->flags = u[2];
		size_t nl = len - 12 < LM_NAME_MAX - 1 ? len - 12 : LM_NAME_MAX - 1;
		memcpy(c->name, c->rx + 12, nl);
		c->name[nl] = 0;
		struct ucred cr;
		socklen_t cl_len = sizeof cr;
		if (!getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl_len))
			c->pid = cr.pid;
		program_path(c->pid, c->path, sizeof c->path);
		c->prog = lm_db_add(&db, c->path, c->name, (int)c->caps,
				    !!(c->flags & LM_CF_PERSISTENT), !!(c->flags & LM_CF_AUTOSTART));
		if (c->prog < 0) {
			reply(c, LM_CONNECT, LM_ERROR_NO_MORE_ITEMS, NULL, 0);
			return;
		}
		c->connected = 1;
		if (c->flags & LM_CF_PERSISTENT)
			save_db();
		uint32_t id = (uint32_t)(c - cl);
		reply(c, LM_CONNECT, LM_OK, &id, sizeof id);
		LOG(0, "applet connected: \"%s\" (%s, pid %d)\n", c->name, c->path, c->pid);
		if (lcd_ok)
			notify(c, LGLCD_NOTIFICATION_DEVICE_ARRIVAL_, LGLCD_DEVICE_QVGA_);
		return;
	}
	case LM_ADMIN: {
		char cmd[512];
		size_t n = len < sizeof cmd - 1 ? len : sizeof cmd - 1;
		memcpy(cmd, c->rx, n);
		cmd[n] = 0;
		handle_admin(c, cmd);
		return;
	}
	}
	if (!c->connected) {
		reply(c, c->h.type, LM_ERROR_SERVICE_NOT_ACTIVE, NULL, 0);
		return;
	}
	const struct lm_program *p = &db.prog[c->prog];
	switch (c->h.type) {
	case LM_DISCONNECT:
		reply(c, LM_DISCONNECT, LM_OK, NULL, 0);
		c->dev_open = 0;
		c->connected = 0;
		dirty = 1;
		break;
	case LM_ICON: {
		uint32_t w = len >= 8 ? u[0] : 0, h = len >= 8 ? u[1] : 0;
		if (w < 1 || h < 1 || w > 64 || h > 64 || len != 8 + w * h * 4) {
			reply(c, LM_ICON, LM_ERROR_INVALID_PARAMETER, NULL, 0);
			break;
		}
		gfx_img_free(&c->icon);
		c->icon.rgba = malloc(w * h * 4);
		if (c->icon.rgba) {
			memcpy(c->icon.rgba, c->rx + 8, w * h * 4);
			c->icon.w = (int)w;
			c->icon.h = (int)h;
		}
		reply(c, LM_ICON, LM_OK, NULL, 0);
		if (menu.open)
			dirty = 1;
		break;
	}
	case LM_FAMILIES:
		c->families = len >= 4 ? u[0] : 0;
		reply(c, LM_FAMILIES, LM_OK, NULL, 0);
		break;
	case LM_ENUMERATE: {
		if (len < 4 || u[0] != 0 || !lcd_ok) {
			reply(c, LM_ENUMERATE, LM_ERROR_NO_MORE_ITEMS, NULL, 0);
			break;
		}
		struct lm_devdesc d = {.family = (uint32_t)db.family_id, .width = TDX_LCD_W,
				       .height = TDX_LCD_H, .bpp = 32, .softbuttons = 7};
		snprintf(d.name, sizeof d.name, "%s", db.family_name);
		reply(c, LM_ENUMERATE, LM_OK, &d, sizeof d);
		break;
	}
	case LM_OPEN: {
		if (len < 8 || (u[0] == 0 && u[1] != 0) || (u[0] == 1 && u[1] != LGLCD_DEVICE_QVGA_ &&
							     u[1] != LGLCD_DEVICE_BW_)) {
			reply(c, LM_OPEN, LM_ERROR_INVALID_PARAMETER, NULL, 0);
			break;
		}
		if (!lcd_ok) {
			reply(c, LM_OPEN, LM_ERROR_DEVICE_NOT_CONNECTED, NULL, 0);
			break;
		}
		if (!p->enabled || !p->dev_enabled) {
			reply(c, LM_OPEN, LM_ERROR_ACCESS_DENIED, NULL, 0);
			break;
		}
		if (!c->frame)
			c->frame = calloc(NPIX, sizeof *c->frame);
		if (!c->frame) {
			reply(c, LM_OPEN, LM_ERROR_NO_MORE_ITEMS, NULL, 0);
			break;
		}
		c->dev_open = 1;
		uint32_t dev = 1;
		reply(c, LM_OPEN, LM_OK, &dev, sizeof dev);
		dirty = 1;
		break;
	}
	case LM_CLOSE:
		c->dev_open = 0;
		c->has_frame = 0;
		reply(c, LM_CLOSE, LM_OK, NULL, 0);
		dirty = 1;
		break;
	case LM_UPDATE: {
		if (len < 12 || !c->dev_open) {
			reply(c, LM_UPDATE, LM_ERROR_DEVICE_NOT_CONNECTED, NULL, 0);
			break;
		}
		uint32_t prio = u[1], fmt = u[2];
		const uint8_t *px = c->rx + 12;
		uint32_t n = len - 12;
		if (fmt == LGLCD_BMP_FORMAT_QVGAx32_ && n == NPIX * 4) {
			for (int i = 0; i < NPIX; i++) /* B G R A -> 0x00RRGGBB */
				c->frame[i] = (uint32_t)px[i * 4] | (uint32_t)px[i * 4 + 1] << 8 |
					      (uint32_t)px[i * 4 + 2] << 16;
		} else if (fmt == LGLCD_BMP_FORMAT_160x43x1_ && n == 160 * 43) {
			/* mono applet on the colour panel: 2x scale, centred, black on white-ish */
			for (int i = 0; i < NPIX; i++)
				c->frame[i] = 0xC8D0D8;
			for (int y = 0; y < 86; y++)
				for (int x = 0; x < 320; x++)
					c->frame[(77 + y) * TDX_LCD_W + x] =
						px[(y / 2) * 160 + x / 2] & 0x80 ? 0x101418 : 0xC8D0D8;
		} else {
			reply(c, LM_UPDATE, LM_ERROR_INVALID_PARAMETER, NULL, 0);
			break;
		}
		int pr = (int)(prio & 0xFF);
		if (pr >= 255 && c->priority < 255)
			c->alert_time = now_ms();
		c->priority = pr;
		c->has_frame = 1;
		if (c - cl == shown_client || pr >= 255)
			dirty = 1;
		if ((prio & 0x80000000u) && c - cl == shown_client && lcd_ok)
			c->wait_sync = 1; /* reply once the frame is on the LCD */
		else
			reply(c, LM_UPDATE, LM_OK, NULL, 0);
		break;
	}
	case LM_FOREGROUND:
		if (len >= 8) {
			c->fg = u[1] != 0;
			c->fg_time = now_ms();
			dirty = 1;
		}
		reply(c, LM_FOREGROUND, LM_OK, NULL, 0);
		break;
	case LM_READBUTTONS:
		reply(c, LM_READBUTTONS, LM_OK, &c->buttons, 4);
		break;
	default:
		reply(c, c->h.type, LM_ERROR_INVALID_PARAMETER, NULL, 0);
	}
}

/* Read what is available; dispatch complete messages. Returns -1 to drop. */
static int client_input(struct client *c)
{
	for (;;) {
		uint8_t *dst;
		size_t want;
		if (c->got < sizeof c->h) {
			dst = (uint8_t *)&c->h + c->got;
			want = sizeof c->h - c->got;
		} else {
			if (c->h.len > LM_MAX_PAYLOAD)
				return -1;
			if (!c->rx && !(c->rx = malloc(LM_MAX_PAYLOAD + 1)))
				return -1;
			dst = c->rx + (c->got - sizeof c->h);
			want = sizeof c->h + c->h.len - c->got;
		}
		if (want) {
			ssize_t r = recv(c->fd, dst, want, MSG_DONTWAIT);
			if (r == 0)
				return -1;
			if (r < 0)
				return errno == EAGAIN || errno == EINTR ? 0 : -1;
			c->got += (uint32_t)r;
			if ((size_t)r < want)
				continue;
		}
		if (c->got == sizeof c->h && c->h.len)
			continue; /* now read the payload */
		handle(c);
		c->got = 0;
		if (c->fd < 0)
			return -1;
	}
}

/* -------------------------------------------------------------- admin */

static int prog_running(int prog)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (cl[i].fd >= 0 && cl[i].connected && cl[i].prog == prog)
			return i;
	return -1;
}

static void spawn(int prog)
{
	const struct lm_program *p = &db.prog[prog];
	if (!p->path[0] || access(p->path, X_OK) || prog_running(prog) >= 0)
		return;
	uint64_t now = now_ms();
	if (spawned_at[prog] && now - spawned_at[prog] < 10000)
		return;
	spawned_at[prog] = now;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			if (!verbose)
				dup2(nul, 2);
		}
		setenv("TDX_LCD_SOCKET", sock_path, 1); /* our socket, also when not the default */
		execl(p->path, p->path, (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		spawned_pid[prog] = pid;
	LOG(0, "started %s (%s), pid %d\n", p->name, p->path, (int)pid);
}

static const struct { const char *exe, *name; } builtin_apps[] = {
	{"tdx-lcd-functionkeys", "Function Keys"},
	{"tdx-lcd-rss", "RSS Feeds"},
	{"tdx-lcd-pictures", "Picture Viewer"},
};

/* Where an applet port is installed: next to us, the source tree's gui/ when
 * run from build/<arch>/, /usr/local/bin, /usr/bin. 0 and *real, or -1. */
static int builtin_path(const char *argv0, const char *exe, char real[PATH_MAX])
{
	char self[PATH_MAX], dirs[4][PATH_MAX];
	int nd = 0;
	if (realpath(argv0, self) || realpath("/proc/self/exe", self)) {
		char *s = strrchr(self, '/');
		if (s) {
			*s = 0;
			snprintf(dirs[nd++], PATH_MAX, "%s", self);
			snprintf(dirs[nd++], PATH_MAX, "%.*s/../../gui", PATH_MAX - 16, self);
		}
	}
	snprintf(dirs[nd++], PATH_MAX, "/usr/local/bin");
	snprintf(dirs[nd++], PATH_MAX, "/usr/bin");
	for (int d = 0; d < nd; d++) {
		char p[PATH_MAX + 64];
		snprintf(p, sizeof p, "%.*s/%s", PATH_MAX - 64, dirs[d], exe);
		if (!access(p, X_OK) && realpath(p, real))
			return 0;
	}
	return -1;
}

/* First run: register the installed 3Dconnexion applet ports like the Windows
 * installer does (Function Keys, RSS Feeds, Picture Viewer; autostart). */
static void seed_programs(const char *argv0)
{
	for (size_t a = 0; a < sizeof builtin_apps / sizeof builtin_apps[0]; a++) {
		char real[PATH_MAX];
		if (builtin_path(argv0, builtin_apps[a].exe, real))
			continue;
		int i = lm_db_add(&db, real, builtin_apps[a].name, LGLCD_APPLET_CAP_QVGA_, 1, 1);
		if (i == 0 && !db.manual_name[0] && strlen(real) < sizeof db.manual_path) {
			memcpy(db.manual_path, real, strlen(real) + 1);
			snprintf(db.manual_name, sizeof db.manual_name, "%s", builtin_apps[a].name);
		}
		LOG(0, "registered %s (%s)\n", builtin_apps[a].name, real);
	}
	if (db.nprog)
		save_db();
}

/* An applet port whose file is gone was moved (install.sh's /usr/local/bin ->
 * a package's /usr/bin): point its entry at where it is now, keeping the
 * user's settings for it, instead of listing a program that can't start. */
static void relocate_builtins(const char *argv0)
{
	int changed = 0;
	for (int i = 0; i < db.nprog; i++) {
		struct lm_program *p = &db.prog[i];
		const char *base = strrchr(p->path, '/');
		base = base ? base + 1 : p->path;
		for (size_t a = 0; a < sizeof builtin_apps / sizeof builtin_apps[0]; a++) {
			char real[PATH_MAX];
			if (strcmp(base, builtin_apps[a].exe) || !access(p->path, F_OK) ||
			    builtin_path(argv0, builtin_apps[a].exe, real) ||
			    lm_db_find(&db, real, p->name) >= 0 || strlen(real) >= sizeof p->path)
				continue;
			LOG(0, "%s moved: %s -> %s\n", p->name, p->path, real);
			if (!strcmp(db.manual_path, p->path) && strlen(real) < sizeof db.manual_path)
				memcpy(db.manual_path, real, strlen(real) + 1);
			memcpy(p->path, real, strlen(real) + 1);
			changed = 1;
		}
	}
	if (changed)
		save_db();
}

static void autostart_all(void)
{
	for (int i = 0; i < db.nprog; i++)
		if (db.prog[i].enabled && db.prog[i].autostart && db.prog[i].dev_enabled)
			spawn(i);
}

static size_t addf(char *buf, size_t o, size_t len, const char *fmt, ...)
{
	if (o >= len)
		return o;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + o, len - o, fmt, ap);
	va_end(ap);
	return n > 0 ? o + (size_t)n : o;
}

static int set_int(const char *k, int v)
{
	struct { const char *name; int *p; int lo, hi; } t[] = {
		{"brightness", &db.brightness, 0, 100}, {"contrast", &db.contrast, 0, 2},
		{"alerts", &db.alerts, 0, 1}, {"algorithm", &db.algorithm, 1, 3},
		{"delay", &db.delay, 1, 20}, {"quickswitch", &db.quickswitch, 0, 1},
		{"invert", &db.invert, 0, 1}, {"show_tray_icon", &db.show_tray_icon, 0, 1},
		{"restore_size", &db.restore_size, 0, 1}, {"show_new_device", &db.show_new_device, 0, 1},
		{"device_balloons", &db.device_balloons, 0, 1},
		{"warning_balloons", &db.warning_balloons, 0, 1}, {"wwidth", &db.wwidth, 200, 4000},
		{"wheight", &db.wheight, 200, 4000},
	};
	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
		if (!strcasecmp(k, t[i].name)) {
			*t[i].p = v < t[i].lo ? t[i].lo : v > t[i].hi ? t[i].hi : v;
			if (t[i].p == &db.brightness && lcd_ok)
				tdx_lcd_set_brightness(&lcd, db.brightness);
			return 0;
		}
	return -1;
}

static void handle_admin(struct client *c, char *cmd)
{
	static char out[8192];
	size_t o = 0, L = sizeof out;
	char *save = NULL, *verb = strtok_r(cmd, " \t\r\n", &save);
	if (!verb)
		verb = "";
	if (!strcmp(verb, "status")) {
		o = addf(out, o, L, "tdx-lcdmgr %s\n", TDX_VERSION);
		o = addf(out, o, L, "device %s %s\n", lcd_ok ? "present" : "absent", db.family_name);
		o = addf(out, o, L, "owner %s\n", hold_fd >= 0 ? "taken-from-tdxd" : "direct");
		o = addf(out, o, L,
			 "settings brightness=%d contrast=%d alerts=%d algorithm=%d delay=%d "
			 "quickswitch=%d invert=%d\n",
			 db.brightness, db.contrast, db.alerts, db.algorithm, db.delay, db.quickswitch,
			 db.invert);
		o = addf(out, o, L,
			 "general show_tray_icon=%d restore_size=%d show_new_device=%d "
			 "device_balloons=%d warning_balloons=%d wwidth=%d wheight=%d\n",
			 db.show_tray_icon, db.restore_size, db.show_new_device, db.device_balloons,
			 db.warning_balloons, db.wwidth, db.wheight);
		o = addf(out, o, L, "database %s\n", db_path);
		o = addf(out, o, L, "shown %d\n", shown_client >= 0 ? cl[shown_client].prog : -1);
		o = addf(out, o, L, "manual %d\n", lm_db_find(&db, db.manual_path, db.manual_name));
		for (int i = 0; i < db.nprog; i++) {
			const struct lm_program *p = &db.prog[i];
			int r = prog_running(i);
			o = addf(out, o, L, "program %d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", i,
				 p->name, p->path, p->enabled, p->autostart, p->persistent, p->allow_fg,
				 p->caps, p->dev_enabled, r >= 0,
				 r >= 0 && (cl[r].flags & LM_CF_CONFIGURABLE) != 0);
		}
	} else if (!strcmp(verb, "set")) {
		char *k = strtok_r(NULL, " \t", &save), *v = strtok_r(NULL, " \t", &save);
		if (!k || !v || set_int(k, atoi(v)))
			o = addf(out, o, L, "error: set KEY VALUE\n");
		else {
			save_db();
			dirty = 1;
			o = addf(out, o, L, "ok\n");
		}
	} else if (!strcmp(verb, "select")) {
		char *v = strtok_r(NULL, " \t", &save);
		int i = v ? atoi(v) : -1;
		if (i < 0 || i >= db.nprog)
			o = addf(out, o, L, "error: no such program\n");
		else {
			select_program(i);
			o = addf(out, o, L, "ok\n");
		}
	} else if (!strcmp(verb, "program")) {
		char *v = strtok_r(NULL, " \t", &save), *what = strtok_r(NULL, " \t", &save);
		char *val = strtok_r(NULL, " \t", &save);
		int i = v ? atoi(v) : -1;
		if (i < 0 || i >= db.nprog || !what) {
			o = addf(out, o, L, "error: program N enabled|autostart|foreground|device 0|1, "
					    "configure, start, remove\n");
		} else {
			struct lm_program *p = &db.prog[i];
			int b = val ? atoi(val) != 0 : 0, r = prog_running(i);
			if (!strcmp(what, "enabled") || !strcmp(what, "device")) {
				int was = p->enabled && p->dev_enabled;
				if (!strcmp(what, "enabled"))
					p->enabled = b;
				else
					p->dev_enabled = b;
				int now = p->enabled && p->dev_enabled;
				if (r >= 0 && was != now) {
					notify(&cl[r], now ? LGLCD_NOTIFICATION_APPLET_ENABLED_
							   : LGLCD_NOTIFICATION_APPLET_DISABLED_, 0);
					if (!now)
						cl[r].dev_open = 0;
				}
				if (now && p->autostart && r < 0)
					spawn(i);
			} else if (!strcmp(what, "autostart")) {
				p->autostart = b;
			} else if (!strcmp(what, "foreground")) {
				p->allow_fg = b;
			} else if (!strcmp(what, "configure")) {
				if (r < 0 || !(cl[r].flags & LM_CF_CONFIGURABLE)) {
					o = addf(out, o, L, "error: not running or not configurable\n");
					goto done;
				}
				send_msg(&cl[r], LM_EV_CONFIGURE, NULL, 0, NULL, 0);
			} else if (!strcmp(what, "start")) {
				spawned_at[i] = 0;
				spawn(i);
			} else if (!strcmp(what, "remove")) {
				if (r >= 0) {
					o = addf(out, o, L, "error: program is running\n");
					goto done;
				}
				memmove(&db.prog[i], &db.prog[i + 1],
					(size_t)(db.nprog - i - 1) * sizeof db.prog[0]);
				db.nprog--;
				for (int j = 0; j < MAX_CLIENTS; j++)
					if (cl[j].prog > i)
						cl[j].prog--;
			} else {
				o = addf(out, o, L, "error: unknown program option '%s'\n", what);
				goto done;
			}
			save_db();
			dirty = 1;
			o = addf(out, o, L, "ok\n");
		}
	} else if (!strcmp(verb, "press")) { /* inject soft buttons: press HEX [HEX...] */
		for (char *v = strtok_r(NULL, " \t", &save); v; v = strtok_r(NULL, " \t", &save))
			soft_buttons((uint32_t)strtoul(v, NULL, 16));
		o = addf(out, o, L, "ok\n");
	} else if (!strcmp(verb, "identify")) {
		identify_until = now_ms() + IDENTIFY_MS;
		dirty = 1;
		o = addf(out, o, L, lcd_ok ? "ok\n" : "error: no LCD\n");
	} else if (!strcmp(verb, "defaults")) {
		lm_db_device_defaults(&db);
		if (lcd_ok)
			tdx_lcd_set_brightness(&lcd, db.brightness);
		save_db();
		dirty = 1;
		o = addf(out, o, L, "ok\n");
	} else if (!strcmp(verb, "reload")) {
		struct lm_db fresh;
		lm_db_defaults(&fresh);
		lm_db_load(&fresh, db_path);
		/* keep running non-persistent programs in the list */
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (cl[i].fd >= 0 && cl[i].connected) {
				const struct lm_program *p = &db.prog[cl[i].prog];
				cl[i].prog = lm_db_add(&fresh, p->path, p->name, p->caps, p->persistent,
						       p->autostart);
			}
		db = fresh;
		if (lcd_ok)
			tdx_lcd_set_brightness(&lcd, db.brightness);
		dirty = 1;
		o = addf(out, o, L, "ok\n");
	} else {
		o = addf(out, o, L,
			 "error: commands: status | set KEY VALUE | select N | program N ... | "
			 "identify | defaults | reload\n");
	}
done:
	reply(c, LM_ADMIN, LM_OK, out, (uint32_t)(o < L ? o : L - 1));
}

/* -------------------------------------------------------------- main */

static void usage(void)
{
	puts("usage: tdx-lcdmgr [-v]... [-s SOCKET] [-d DATABASE] [-t TDXD_SOCKET] [-V]\n"
	     "  LCD Manager for the SpacePilot PRO LCD: serves LCD applets (libtdxlcd,\n"
	     "  tdxlcd.py) and takes the LCD over from tdxd while it runs.\n"
	     "  -s  applet socket (default $XDG_RUNTIME_DIR/tdx-lcd.sock)\n"
	     "  -d  settings database (default ~/.config/tdxd/lcdmgr.ini)\n"
	     "  -t  tdxd control socket (default /run/tdxd.sock)\n"
	     "  -F  virtual LCD: write frames to FILE.ppm instead of the device (testing)\n"
	     "  -A  3DxWare artwork directory (tdx-extract-3dxware-ui; default: the usual places)\n"
	     "  -N  don't register the installed applets on first run");
}

int main(int argc, char **argv)
{
	int opt;
	sock_path[0] = 0;
	lm_db_default_path(db_path, sizeof db_path);
	while ((opt = getopt(argc, argv, "vs:d:t:F:A:NVh")) != -1) {
		switch (opt) {
		case 'v': verbose++; break;
		case 's': snprintf(sock_path, sizeof sock_path, "%s", optarg); break;
		case 'd': snprintf(db_path, sizeof db_path, "%s", optarg); break;
		case 't': snprintf(tdxd_sock, sizeof tdxd_sock, "%s", optarg); break;
		case 'F': snprintf(fake_path, sizeof fake_path, "%s", optarg); break;
		case 'A': snprintf(assets_dir, sizeof assets_dir, "%s", optarg); break;
		case 'N': no_seed = 1; break;
		case 'V': puts("tdx-lcdmgr " TDX_VERSION); return 0;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}
	if (!sock_path[0]) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (rt && *rt)
			snprintf(sock_path, sizeof sock_path, "%s/tdx-lcd.sock", rt);
		else
			snprintf(sock_path, sizeof sock_path, "/tmp/tdx-lcd-%u.sock", (unsigned)getuid());
	}
	for (int i = 0; i < MAX_CLIENTS; i++) {
		cl[i].fd = -1;
		cl[i].prog = -1;
	}
	lm_db_defaults(&db);
	int rc = lm_db_load(&db, db_path);
	if (rc == -ENOENT && !no_seed)
		seed_programs(argv[0]);
	else if (!rc && !no_seed)
		relocate_builtins(argv[0]);
	if (rc && rc != -ENOENT)
		LOG(0, "cannot read %s: %s\n", db_path, strerror(-rc));

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN); /* autostarted applets are not waited for */

	struct sockaddr_un a = {.sun_family = AF_UNIX};
	snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	unlink(sock_path);
	if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 ||
	    listen(listen_fd, 16) < 0) {
		fprintf(stderr, "tdx-lcdmgr: %s: %s\n", sock_path, strerror(errno));
		return 1;
	}
	chmod(sock_path, 0600);
	LOG(0, "applet socket %s, database %s\n", sock_path, db_path);

	lcd_try();
	autostart_all();
	while (!g_quit) {
		lcd_try();
		struct pollfd pfd[MAX_CLIENTS + 3];
		int np = 0;
		pfd[np++] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
		int lcd_idx = -1, hold_idx = -1, cbase = np;
		for (int i = 0; i < MAX_CLIENTS; i++)
			pfd[np++] = (struct pollfd){.fd = cl[i].fd, .events = POLLIN};
		if (lcd_ok && !fake_path[0] && lcd.in_pending) {
			lcd_idx = np;
			pfd[np++] = (struct pollfd){.fd = lcd.fd, .events = POLLOUT};
		}
		if (hold_fd >= 0) {
			hold_idx = np;
			pfd[np++] = (struct pollfd){.fd = hold_fd, .events = POLLIN};
		}
		int timeout = dirty ? FRAME_MIN_MS : (menu.open || db.algorithm != 1 || identify_until)
							     ? 100 : 1000;
		int r = poll(pfd, (nfds_t)np, timeout);
		uint64_t now = now_ms();
		if (lm_menu_tick(&menu, now))
			dirty = 1;
		if (identify_until && now >= identify_until) {
			identify_until = 0;
			dirty = 1;
		}
		if (r > 0) {
			if (pfd[0].revents & POLLIN) {
				int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
				int slot = -1;
				for (int i = 0; i < MAX_CLIENTS && fd >= 0; i++)
					if (cl[i].fd < 0) {
						slot = i;
						break;
					}
				if (slot < 0 && fd >= 0)
					close(fd);
				else if (fd >= 0)
					cl[slot].fd = fd;
			}
			for (int i = 0; i < MAX_CLIENTS; i++)
				if (cl[i].fd >= 0 && (pfd[cbase + i].revents & (POLLIN | POLLHUP | POLLERR)))
					if (client_input(&cl[i]) < 0) {
						if (cl[i].connected)
							LOG(0, "applet gone: \"%s\"\n", cl[i].name);
						client_close(&cl[i]);
					}
			if (lcd_idx >= 0 && (pfd[lcd_idx].revents & (POLLOUT | POLLERR | POLLHUP))) {
				uint32_t b = 0;
				int k = tdx_lcd_buttons_reap(&lcd, &b);
				if (k > 0)
					soft_buttons(b);
				else if (k == -ENODEV) {
					tdx_lcd_close(&lcd);
					lcd_ok = 0;
					notify_all(LGLCD_NOTIFICATION_DEVICE_REMOVAL_, LGLCD_DEVICE_QVGA_);
				}
			}
			if (hold_idx >= 0 && (pfd[hold_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
				char junk[64];
				if (read(hold_fd, junk, sizeof junk) <= 0) {
					close(hold_fd); /* tdxd restarted; re-hand the LCD over later */
					hold_fd = -1;
				}
			}
		}
		if (lcd_ok && hold_fd < 0 && !fake_path[0])
			hold_tdxd(); /* tdxd came (back): make sure it leaves the LCD alone */
		render();
	}
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (cl[i].fd >= 0) {
			/* an applet we started goes with us (as under systemd, where
			 * stopping the unit ends its cgroup); one still connected is
			 * certainly that process, never a reused pid */
			int p = cl[i].prog;
			if (cl[i].connected && p >= 0 && p < LM_MAX_PROG && spawned_pid[p] > 0 &&
			    cl[i].pid == spawned_pid[p]) {
				LOG(1, "stopping %s (pid %d)\n", db.prog[p].name, cl[i].pid);
				kill(cl[i].pid, SIGTERM);
			}
			notify(&cl[i], LGLCD_NOTIFICATION_CLOSE_CONNECTION_, 0);
			client_close(&cl[i]);
		}
	if (lcd_ok && !fake_path[0])
		tdx_lcd_close(&lcd);
	if (hold_fd >= 0)
		close(hold_fd); /* tdxd takes the LCD back */
	close(listen_fd);
	unlink(sock_path);
	return 0;
}
