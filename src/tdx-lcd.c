/*
 * tdx-lcd - drive the SpacePilot Pro LCD directly (stop tdxd or set lcd=no first).
 *
 *   tdx-lcd image FILE.ppm        show a 320x240 binary PPM
 *   tdx-lcd text "line1" ...      show up to 8 text lines
 *   tdx-lcd fill RRGGBB           solid colour (test pattern: "bars")
 *   tdx-lcd brightness [0-100]    get / set backlight
 *   tdx-lcd dump FILE.bin         write the raw USB frame (header+pixels), no device
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "config.h"
#include "lcd.h"
#include "process.h"

static uint32_t px[TDX_LCD_W * TDX_LCD_H];
static struct tdx_fb fb = {TDX_LCD_W, TDX_LCD_H, px};

static void bars(void)
{
	static const uint32_t c[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
				      0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
	for (int i = 0; i < 8; i++)
		gfx_fill(&fb, i * 40, 0, 40, TDX_LCD_H, c[i]);
	gfx_text(&fb, 10, 110, "TDXD LCD TEST", 3, 0x808080);
}

static int usage(void)
{
	fputs("usage: tdx-lcd [-d VID:PID] image FILE.ppm | text LINE... | fill RRGGBB|bars |\n"
	      "               brightness [0-100] | dump OUT.bin | preview OUT.ppm [CONF]\n",
	      stderr);
	return 2;
}

int main(int argc, char **argv)
{
	unsigned vid = 0x046D, pid = 0xC629;
	int ai = 1;
	if (argc > 2 && !strcmp(argv[1], "-d")) {
		if (sscanf(argv[2], "%x:%x", &vid, &pid) != 2)
			return usage();
		ai = 3;
	}
	if (ai >= argc)
		return usage();
	const char *cmd = argv[ai++];

	if (!strcmp(cmd, "preview")) { /* render tdxd's key page to a PPM, no device */
		if (ai >= argc)
			return usage();
		struct tdx_config cfg;
		tdx_config_defaults(&cfg);
		tdx_config_load(&cfg, ai + 1 < argc ? argv[ai + 1] : "/etc/tdxd.conf");
		tdx_render_keys_page(&fb, &cfg, "SpacePilot PRO", 0);
		FILE *f = fopen(argv[ai], "wb");
		if (!f) {
			perror(argv[ai]);
			return 1;
		}
		fprintf(f, "P6\n%d %d\n255\n", TDX_LCD_W, TDX_LCD_H);
		for (int i = 0; i < TDX_LCD_W * TDX_LCD_H; i++) {
			fputc((int)(px[i] >> 16) & 0xFF, f);
			fputc((int)(px[i] >> 8) & 0xFF, f);
			fputc((int)px[i] & 0xFF, f);
		}
		fclose(f);
		return 0;
	}

	if (!strcmp(cmd, "dump")) {
		if (ai >= argc)
			return usage();
		static uint8_t frame[TDX_LCD_FRAME];
		bars();
		tdx_lcd_header(frame);
		tdx_lcd_pack(px, frame + TDX_LCD_HDR);
		FILE *f = fopen(argv[ai], "wb");
		if (!f || fwrite(frame, 1, sizeof frame, f) != sizeof frame) {
			perror(argv[ai]);
			return 1;
		}
		fclose(f);
		return 0;
	}

	static struct tdx_lcd lcd;
	int rc = tdx_lcd_open(&lcd, (uint16_t)vid, (uint16_t)pid);
	if (rc) {
		fprintf(stderr, "tdx-lcd: cannot open LCD %04x:%04x: %s\n", vid, pid, strerror(-rc));
		if (rc == -EBUSY)
			fputs("  (interface busy: stop tdxd or set lcd = no)\n", stderr);
		if (rc == -EACCES)
			fputs("  (permission denied: install the udev rule or run as root)\n", stderr);
		return 1;
	}

	if (!strcmp(cmd, "brightness")) {
		if (ai < argc)
			rc = tdx_lcd_set_brightness(&lcd, atoi(argv[ai]));
		else if ((rc = tdx_lcd_get_brightness(&lcd)) >= 0) {
			printf("%d\n", rc);
			rc = 0;
		}
	} else {
		if (!strcmp(cmd, "image") && ai < argc) {
			rc = gfx_load_ppm(&fb, argv[ai]);
		} else if (!strcmp(cmd, "text")) {
			gfx_fill(&fb, 0, 0, TDX_LCD_W, TDX_LCD_H, 0x000000);
			for (int i = 0; ai < argc && i < 8; i++, ai++)
				gfx_text(&fb, 4, 4 + i * 29, argv[ai], 3, 0xFFFFFF);
		} else if (!strcmp(cmd, "fill") && ai < argc) {
			if (!strcmp(argv[ai], "bars"))
				bars();
			else
				gfx_fill(&fb, 0, 0, TDX_LCD_W, TDX_LCD_H, (uint32_t)strtoul(argv[ai], NULL, 16));
		} else {
			tdx_lcd_close(&lcd);
			return usage();
		}
		if (!rc)
			rc = tdx_lcd_show(&lcd, px);
	}
	tdx_lcd_close(&lcd);
	if (rc) {
		fprintf(stderr, "tdx-lcd: %s\n", strerror(-rc));
		return 1;
	}
	return 0;
}
