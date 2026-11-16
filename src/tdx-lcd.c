/*
 * tdx-lcd - drive a 3Dconnexion LCD directly (stop tdxd or set lcd = no first).
 *
 * Supported panels (auto-detected, or pick one with -d VID:PID):
 *   046d:c629  SpacePilot Pro         320x240, raw RGB565 frames
 *   256f:c633  SpaceMouse Enterprise  640x150, deflate-compressed RGB565 frames
 *   256f:c63c  Giotto                1280x416, deflate-compressed 24 bpp, double-buffered
 *
 *   tdx-lcd image FILE.ppm        show a binary PPM (cropped / padded to the panel)
 *   tdx-lcd text "line1" ...      show text lines
 *   tdx-lcd fill RRGGBB|bars      solid colour or colour bars
 *   tdx-lcd brightness [0-100]    get (SpacePilot Pro) / set (SpacePilot Pro, Enterprise)
 *   tdx-lcd ping                  SpacePilot Pro vendor request 0x01
 *   tdx-lcd dump OUT.bin          write the USB transfer for 'bars' to a file, no device
 *   tdx-lcd preview OUT.ppm [CONF] render tdxd's key page to a PPM, no device
 *   tdx-lcd slide [-s PX] P1.ppm P2.ppm [P3 P4]   Giotto: slide through 2-4 pages,
 *                                 PX pixels per 25 ms step (default 64)
 *   tdx-lcd move [-s PX] [-w] MOVING.ppm BACKGROUND.ppm   Giotto: move the image
 *                                 down off the panel, uncovering the background
 *                                 (-w: Windows-exact, never shows the result)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

#include "config.h"
#include "deflate.h"
#include "gfx.h"
#include "hidraw.h"
#include "lcd.h"
#include "lcd_ent.h"
#include "lcd_giotto.h"
#include "process.h"
#include "usbfs.h"

static uint32_t px[TDX_GIO_PIXELS]; /* largest panel */
static struct tdx_fb fb;
static int kind; /* TDX_LCD_QVGA, TDX_LCD_ENT640 or TDX_LCD_GIO1280 */

static void bars(void)
{
	static const uint32_t c[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
				      0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
	int bw = fb.w / 8;
	for (int i = 0; i < 8; i++)
		gfx_fill(&fb, i * bw, 0, i == 7 ? fb.w - 7 * bw : bw, fb.h, c[i]);
	gfx_text(&fb, 10, fb.h / 2 - 10, "TDXD LCD TEST", 3, 0x808080);
}

static int usage(void)
{
	fputs("usage: tdx-lcd [-d VID:PID] image FILE.ppm | text LINE... | fill RRGGBB|bars |\n"
	      "               brightness [0-100] | ping | dump OUT.bin | preview OUT.ppm [CONF] |\n"
	      "               slide [-s PX] PAGE1.ppm PAGE2.ppm [PAGE3.ppm PAGE4.ppm]   (Giotto)\n"
	      "               move [-s PX] [-w] MOVING.ppm BACKGROUND.ppm                (Giotto)\n"
	      "  panels: 046d:c629 SpacePilot Pro (320x240), 256f:c633 SpaceMouse Enterprise (640x150),\n"
	      "          256f:c63c Giotto (1280x416)\n",
	      stderr);
	return 2;
}

static void set_kind(int k)
{
	kind = k;
	fb.px = px;
	fb.w = k == TDX_LCD_GIO1280 ? TDX_GIO_W : k == TDX_LCD_ENT640 ? TDX_ENT_W : TDX_LCD_W;
	fb.h = k == TDX_LCD_GIO1280 ? TDX_GIO_H : k == TDX_LCD_ENT640 ? TDX_ENT_H : TDX_LCD_H;
}

static int write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", fb.w, fb.h);
	for (int i = 0; i < fb.w * fb.h; i++) {
		fputc((int)(px[i] >> 16) & 0xFF, f);
		fputc((int)(px[i] >> 8) & 0xFF, f);
		fputc((int)px[i] & 0xFF, f);
	}
	fclose(f);
	return 0;
}

/* Enterprise brightness goes over HID (feature report 0x11), not usbfs. */
static int ent_brightness(uint16_t vid, uint16_t pid, int percent)
{
	struct tdx_hid_node nodes[8];
	int n = tdx_hidraw_scan(nodes, 8);
	for (int i = 0; i < n; i++) {
		if (nodes[i].vid != vid || nodes[i].pid != pid)
			continue;
		uint8_t rep[2];
		if (!tdx_lcd_intensity_report(nodes[i].def, percent, rep))
			return -EINVAL;
		int fd = open(nodes[i].devnode, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			return -errno;
		int rc = ioctl(fd, HIDIOCSFEATURE(2), rep) < 0 ? -errno : 0;
		close(fd);
		return rc;
	}
	return -ENODEV;
}

static int open_error(unsigned vid, unsigned pid, int rc)
{
	fprintf(stderr, "tdx-lcd: cannot open LCD %04x:%04x: %s\n", vid, pid, strerror(-rc));
	if (rc == -EBUSY)
		fputs("  (interface busy: stop tdxd or set lcd = no)\n", stderr);
	if (rc == -EACCES)
		fputs("  (permission denied: install the udev rule or run as root)\n", stderr);
	return 1;
}

int main(int argc, char **argv)
{
	unsigned vid = 0, pid = 0;
	int ai = 1;
	if (argc > 2 && !strcmp(argv[1], "-d")) {
		if (sscanf(argv[2], "%x:%x", &vid, &pid) != 2)
			return usage();
		ai = 3;
	}
	if (ai >= argc)
		return usage();
	const char *cmd = argv[ai++];

	/* Pick the panel: explicit -d, else the first LCD device present, else SpacePilot Pro. */
	if (!vid) {
		static const uint16_t cand[][2] = {{0x046D, 0xC629}, {0x256F, 0xC633}, {0x256F, 0xC63C}};
		char node[64];
		vid = cand[0][0];
		pid = cand[0][1];
		for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++)
			if (!tdx_usb_find_node(cand[i][0], cand[i][1], node, sizeof node)) {
				vid = cand[i][0];
				pid = cand[i][1];
				break;
			}
	}
	const struct tdx_device_def *def = tdx_device_lookup((uint16_t)vid, (uint16_t)pid);
	if (!def || def->lcd == TDX_LCD_NONE) {
		fprintf(stderr, "tdx-lcd: %04x:%04x has no supported LCD\n", vid, pid);
		return 1;
	}
	set_kind(def->lcd);

	if (!strcmp(cmd, "preview")) {
		if (ai >= argc)
			return usage();
		struct tdx_config cfg;
		tdx_config_defaults(&cfg);
		tdx_config_load(&cfg, ai + 1 < argc ? argv[ai + 1] : "/etc/tdxd.conf");
		if (kind == TDX_LCD_ENT640 || kind == TDX_LCD_GIO1280)
			tdx_render_ent_page(&fb, &cfg, def->name, 0, kind == TDX_LCD_GIO1280 ? tdx_config_lcd_pages(&cfg) : 1);
		else
			tdx_render_keys_page(&fb, &cfg, def->name, 0);
		return write_ppm(argv[ai]);
	}

	if (!strcmp(cmd, "dump")) {
		if (ai >= argc)
			return usage();
		bars();
		FILE *f = fopen(argv[ai], "wb");
		if (!f) {
			perror(argv[ai]);
			return 1;
		}
		int ok;
		if (kind == TDX_LCD_GIO1280) {
			/* command 0x20 for buffer 1 (first back buffer) + payload */
			static uint8_t raw[TDX_GIO_RAW], comp[TDX_DEFLATE_BOUND(TDX_GIO_RAW)];
			uint8_t pkt[TDX_GIO_PKT];
			tdx_gio_pack(px, raw, 0);
			long n = tdx_deflate_fixed(raw, sizeof raw, comp, sizeof comp);
			tdx_gio_cmd_write(pkt, 1, (uint32_t)(n > 0 ? n : 0));
			ok = n > 0 && fwrite(pkt, 1, sizeof pkt, f) == sizeof pkt &&
			     fwrite(comp, 1, (size_t)n, f) == (size_t)n;
		} else if (kind == TDX_LCD_ENT640) {
			static uint16_t p565[TDX_ENT_PIXELS];
			static uint8_t comp[TDX_DEFLATE_BOUND(TDX_ENT_RAW)];
			uint8_t pkt[TDX_ENT_PKT];
			tdx_ent_pack(px, p565, 0);
			long n = tdx_ent_encode(p565, TDX_ENT_OP_FULL, pkt, comp, sizeof comp);
			ok = n > 0 && fwrite(pkt, 1, sizeof pkt, f) == sizeof pkt &&
			     fwrite(comp, 1, (size_t)n, f) == (size_t)n;
		} else {
			static uint8_t frame[TDX_LCD_FRAME];
			tdx_lcd_header(frame);
			tdx_lcd_pack(px, frame + TDX_LCD_HDR);
			ok = fwrite(frame, 1, sizeof frame, f) == sizeof frame;
		}
		fclose(f);
		if (!ok) {
			fprintf(stderr, "tdx-lcd: writing %s failed\n", argv[ai]);
			return 1;
		}
		return 0;
	}

	int rc = 0;
	if (!strcmp(cmd, "brightness") && kind == TDX_LCD_ENT640) {
		if (ai >= argc) {
			fputs("tdx-lcd: the Enterprise brightness cannot be read back, only set\n", stderr);
			return 1;
		}
		rc = ent_brightness((uint16_t)vid, (uint16_t)pid, atoi(argv[ai]));
		if (rc)
			fprintf(stderr, "tdx-lcd: %s\n", strerror(-rc));
		return rc ? 1 : 0;
	}
	if (kind == TDX_LCD_GIO1280 && (!strcmp(cmd, "brightness") || !strcmp(cmd, "ping"))) {
		fputs("tdx-lcd: no brightness/ping request is known for the Giotto LCD\n", stderr);
		return 1;
	}
	if (!strcmp(cmd, "ping") && kind == TDX_LCD_ENT640) {
		fputs("tdx-lcd: ping is a SpacePilot Pro request\n", stderr);
		return 1;
	}

	if (!strcmp(cmd, "slide")) {
		if (kind != TDX_LCD_GIO1280) {
			fputs("tdx-lcd: slide is a Giotto LCD operation\n", stderr);
			return 1;
		}
		int step = 64;
		if (ai + 1 < argc && !strcmp(argv[ai], "-s")) {
			step = atoi(argv[ai + 1]);
			ai += 2;
		}
		int np = argc - ai;
		if (np < 2 || np > TDX_GIO_MAX_PAGES || step <= 0)
			return usage();
		static uint32_t *pages[TDX_GIO_MAX_PAGES];
		for (int i = 0; i < np; i++) {
			pages[i] = calloc(TDX_GIO_PIXELS, sizeof(uint32_t));
			struct tdx_fb pf = {TDX_GIO_W, TDX_GIO_H, pages[i]};
			if (!pages[i] || (rc = gfx_load_ppm(&pf, argv[ai + i]))) {
				fprintf(stderr, "tdx-lcd: %s: %s\n", argv[ai + i], strerror(rc ? -rc : ENOMEM));
				return 1;
			}
		}
		static struct tdx_giolcd gl;
		if ((rc = tdx_giolcd_open(&gl, (uint16_t)vid, (uint16_t)pid)))
			return open_error(vid, pid, rc);
		/* static first page (fills the rows outside the slide band), then slide */
		rc = tdx_giolcd_show(&gl, pages[0]);
		if (!rc)
			rc = tdx_giolcd_set_pages(&gl, (const uint32_t *const *)pages, np);
		int end = (np - 1) * TDX_GIO_W;
		for (int x = step; !rc && x <= end + step - 1; x += step) {
			rc = tdx_giolcd_slide(&gl, x < end ? x : end);
			usleep(TDX_GIO_SLIDE_STEP_MS * 1000);
		}
		tdx_giolcd_close(&gl);
		if (rc) {
			fprintf(stderr, "tdx-lcd: %s\n", strerror(-rc));
			return 1;
		}
		return 0;
	}

	if (!strcmp(cmd, "move")) {
		if (kind != TDX_LCD_GIO1280) {
			fputs("tdx-lcd: move is a Giotto LCD operation\n", stderr);
			return 1;
		}
		int step = 16, present = 1;
		for (;;) {
			if (ai + 1 < argc && !strcmp(argv[ai], "-s")) {
				step = atoi(argv[ai + 1]);
				ai += 2;
			} else if (ai < argc && !strcmp(argv[ai], "-w")) {
				present = 0;
				ai++;
			} else
				break;
		}
		if (argc - ai != 2 || step <= 0)
			return usage();
		static uint32_t *img[2];
		for (int i = 0; i < 2; i++) {
			img[i] = calloc(TDX_GIO_PIXELS, sizeof(uint32_t));
			struct tdx_fb pf = {TDX_GIO_W, TDX_GIO_H, img[i]};
			if (!img[i] || (rc = gfx_load_ppm(&pf, argv[ai + i]))) {
				fprintf(stderr, "tdx-lcd: %s: %s\n", argv[ai + i], strerror(rc ? -rc : ENOMEM));
				return 1;
			}
		}
		static struct tdx_giolcd gl;
		if ((rc = tdx_giolcd_open(&gl, (uint16_t)vid, (uint16_t)pid)))
			return open_error(vid, pid, rc);
		/* moving image shown statically, then pushed down to y = 416 */
		rc = tdx_giolcd_show(&gl, img[0]);
		if (!rc)
			rc = tdx_giolcd_set_moving(&gl, img[0], img[1]);
		for (int y = step; !rc && y < TDX_GIO_MOVE_MAX_Y + step; y += step) {
			rc = tdx_giolcd_move(&gl, y < TDX_GIO_MOVE_MAX_Y ? y : TDX_GIO_MOVE_MAX_Y, present);
			usleep(TDX_GIO_SLIDE_STEP_MS * 1000);
		}
		if (!rc && present)
			rc = tdx_giolcd_move_finish(&gl);
		tdx_giolcd_close(&gl);
		if (rc) {
			fprintf(stderr, "tdx-lcd: %s\n", strerror(-rc));
			return 1;
		}
		return 0;
	}

	/* Render first, so a bad argument doesn't claim the USB interface. */
	int draw = 1;
	if (!strcmp(cmd, "image") && ai < argc) {
		if ((rc = gfx_load_ppm(&fb, argv[ai]))) {
			fprintf(stderr, "tdx-lcd: %s: %s\n", argv[ai], strerror(-rc));
			return 1;
		}
	} else if (!strcmp(cmd, "text")) {
		gfx_fill(&fb, 0, 0, fb.w, fb.h, 0x000000);
		for (int i = 0; ai < argc && 4 + i * 29 + 21 <= fb.h; i++, ai++)
			gfx_text(&fb, 4, 4 + i * 29, argv[ai], 3, 0xFFFFFF);
	} else if (!strcmp(cmd, "fill") && ai < argc) {
		if (!strcmp(argv[ai], "bars"))
			bars();
		else
			gfx_fill(&fb, 0, 0, fb.w, fb.h, (uint32_t)strtoul(argv[ai], NULL, 16));
	} else if (!strcmp(cmd, "brightness") || !strcmp(cmd, "ping")) {
		draw = 0;
	} else {
		return usage();
	}

	if (kind == TDX_LCD_GIO1280) {
		static struct tdx_giolcd gl;
		if ((rc = tdx_giolcd_open(&gl, (uint16_t)vid, (uint16_t)pid)))
			return open_error(vid, pid, rc);
		rc = tdx_giolcd_show(&gl, px);
		tdx_giolcd_close(&gl);
	} else if (kind == TDX_LCD_ENT640) {
		static struct tdx_entlcd el;
		if ((rc = tdx_entlcd_open(&el, (uint16_t)vid, (uint16_t)pid)))
			return open_error(vid, pid, rc);
		rc = tdx_entlcd_show(&el, px, 0); /* first frame after open: opcode 0x15 */
		tdx_entlcd_close(&el);
	} else {
		static struct tdx_lcd lcd;
		if ((rc = tdx_lcd_open(&lcd, (uint16_t)vid, (uint16_t)pid)))
			return open_error(vid, pid, rc);
		if (!strcmp(cmd, "ping")) {
			if (!(rc = tdx_lcd_ping(&lcd)))
				puts("LCD answered");
		} else if (!strcmp(cmd, "brightness")) {
			if (ai < argc)
				rc = tdx_lcd_set_brightness(&lcd, atoi(argv[ai]));
			else if ((rc = tdx_lcd_get_brightness(&lcd)) >= 0) {
				printf("%d\n", rc);
				rc = 0;
			}
		} else if (draw) {
			if (tdx_lcd_wait_ready(&lcd, 20) != 0)
				fputs("tdx-lcd: no answer to ping, sending frame anyway\n", stderr);
			rc = tdx_lcd_show(&lcd, px);
		}
		tdx_lcd_close(&lcd);
	}
	if (rc) {
		fprintf(stderr, "tdx-lcd: %s\n", strerror(-rc));
		return 1;
	}
	return 0;
}
