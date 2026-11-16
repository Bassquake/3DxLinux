/* Host-side tests for the hardware-independent parts of tdxd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "devices.h"
#include "lcd.h"
#include "process.h"
#include "report.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int find_btn(const struct tdx_device_def *d, const char *v3dk)
{
	for (int i = 0; i < d->nbuttons; i++)
		if (d->buttons[i].v3dk && !strcmp(d->buttons[i].v3dk, v3dk))
			return i;
	return -1;
}

static void test_device_table(void)
{
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	CHECK(spp && !strcmp(spp->name, "SpacePilot PRO"));
	CHECK(spp && spp->nbuttons == 31);
	CHECK(spp && spp->lcd == TDX_LCD_QVGA);
	CHECK(spp && spp->hid_iface == 1);
	CHECK(tdx_device_lookup(0x256F, 0xC635) != NULL); /* SpaceMouse Compact */
	CHECK(tdx_device_lookup(0x046D, 0xC627) != NULL); /* legacy SpaceExplorer */
	CHECK(tdx_device_lookup(0x1234, 0x5678) == NULL);
}

static void test_axes(void)
{
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	struct tdx_state st;
	memset(&st, 0, sizeof st);
	const uint8_t t[7] = {0x01, 0x10, 0x00, 0xF0, 0xFF, 0x5E, 0x01}; /* 16, -16, 350 */
	const uint8_t r[7] = {0x02, 0xA2, 0xFE, 0x00, 0x00, 0x01, 0x00}; /* -350, 0, 1 */
	CHECK(tdx_report_feed(&st, spp, t, sizeof t) == 1);
	CHECK(tdx_report_feed(&st, spp, r, sizeof r) == 1);
	CHECK(st.axis[0] == 16 && st.axis[1] == -16 && st.axis[2] == 350);
	CHECK(st.axis[3] == -350 && st.axis[4] == 0 && st.axis[5] == 1);

	/* combined 13-byte report (newer devices) */
	const uint8_t c[13] = {0x01, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0};
	tdx_report_feed(&st, NULL, c, sizeof c);
	CHECK(st.axis[0] == 1 && st.axis[5] == 6);

	struct tdx_config cfg;
	tdx_config_defaults(&cfg);
	int16_t raw[6] = {1, -1, 100, 0, -200, 3};
	int out[6];
	tdx_process_axes(&cfg, raw, out);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 100 && out[4] == -200 && out[5] == 3);
	cfg.axis_map[1] = 2; cfg.axis_map[2] = 1; cfg.axis_scale[2] = -1.0; cfg.sensitivity = 2.0;
	tdx_process_axes(&cfg, raw, out);
	CHECK(out[1] == 200 && out[2] == 0);
	cfg.dominant = 1;
	tdx_process_axes(&cfg, raw, out);
	CHECK(out[4] == -400 && out[1] == 0);
}

static void test_buttons(void)
{
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	struct tdx_state st;
	memset(&st, 0, sizeof st);
	int fit = find_btn(spp, "V3DK_FIT"), esc = find_btn(spp, "V3DK_ESC"), minus = find_btn(spp, "V3DK_MINUS");
	CHECK(fit >= 0 && esc >= 0 && minus >= 0);
	const uint8_t b1[5] = {0x03, 0x02, 0x00, 0x40, 0x40}; /* FIT, ESC, MINUS */
	CHECK(tdx_report_feed(&st, spp, b1, sizeof b1) == 2);
	CHECK(st.btn[fit] && st.btn[esc] && st.btn[minus]);
	const uint8_t b0[5] = {0x03, 0, 0, 0, 0};
	CHECK(tdx_report_feed(&st, spp, b0, sizeof b0) == 2);
	CHECK(!st.btn[fit] && !st.btn[esc]);
	CHECK(tdx_report_feed(&st, spp, b0, sizeof b0) == 0); /* no change */

	/* SpaceMouse Enterprise: report 0x1C carries pressed button numbers */
	const struct tdx_device_def *ent = tdx_device_lookup(0x256F, 0xC633);
	CHECK(ent != NULL);
	if (ent) {
		memset(&st, 0, sizeof st);
		int top = find_btn(ent, "V3DK_TOP"), menu = find_btn(ent, "V3DK_MENU");
		const uint8_t n[13] = {0x1C, 3, 0, 1, 0};
		CHECK(tdx_report_feed(&st, ent, n, sizeof n) == 2);
		CHECK(st.btn[top] && st.btn[menu]);
		int lp = find_btn(ent, "V3DK_BOTTOM"); /* long-press of TOP (mask 0x4) */
		const uint8_t l[5] = {0x1D, 0x04, 0, 0, 0};
		CHECK(tdx_report_feed(&st, ent, l, sizeof l) == 2);
		CHECK(lp >= 0 && st.btn[lp]);
	}

	/* generic numbering for devices without a table */
	memset(&st, 0, sizeof st);
	const uint8_t g[3] = {0x03, 0x01, 0x80};
	tdx_report_feed(&st, NULL, g, sizeof g);
	CHECK(st.btn[0] && st.btn[15] && !st.btn[1]);
}

static void test_lcd(void)
{
	uint8_t h[TDX_LCD_HDR];
	tdx_lcd_header(h);
	const uint8_t exp[16] = {0x10, 0x0F, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00,
				 0x00, 0x00, 0x00, 0x3F, 0x01, 0xEF, 0x00, 0x0F};
	CHECK(!memcmp(h, exp, 16));
	CHECK(h[0x10] == 0x10 && h[0x1FF] == 0xFF);
	CHECK(TDX_LCD_FRAME == 0x25a00);
	uint8_t led[2];
	CHECK(tdx_led_report(tdx_device_lookup(0x046D, 0xC629), 1, led) == 2 && led[0] == 4 && led[1] == 1);
	CHECK(tdx_led_report(tdx_device_lookup(0x256F, 0xC62B), 0, led) == 2 && led[1] == 0);
	CHECK(tdx_led_report(tdx_device_lookup(0x256F, 0xC63C), 1, led) == 0);

	static uint32_t px[TDX_LCD_W * TDX_LCD_H];
	static uint8_t out[TDX_LCD_W * TDX_LCD_H * 2];
	px[0] = 0xFF0000;                      /* (0,0) red   */
	px[1] = 0x00FF00;                      /* (1,0) green */
	px[TDX_LCD_W] = 0x0000FF;              /* (0,1) blue  */
	px[TDX_LCD_W * 240 - 1] = 0xFFFFFF;    /* (319,239)   */
	tdx_lcd_pack(px, out);
	CHECK(out[0] == 0x00 && out[1] == 0xF8);           /* red  = 0xF800 */
	CHECK(out[2] == 0x1F && out[3] == 0x00);           /* (0,1) blue = 0x001F, column-major */
	CHECK(out[2 * 240] == 0xE0 && out[2 * 240 + 1] == 0x07); /* (1,0) green = 0x07E0 */
	CHECK(out[sizeof out - 2] == 0xFF && out[sizeof out - 1] == 0xFF);
}

static void test_config(void)
{
	const char *p = "test_tdxd.conf";
	FILE *f = fopen(p, "w");
	fputs("# test\nsensitivity = 0.5\ninvert_ry = yes   # inline comment\nspnav = no # off\nmap_ty = tz\n"
	      "key_V3DK_ESC = KEY_ESC\nkey_V3DK_CTRL = LEFTCTRL + C\nlcd_label_3 = Measure\n", f);
	fclose(f);
	struct tdx_config c;
	tdx_config_defaults(&c);
	CHECK(tdx_config_load(&c, p) == 0);
	remove(p);
	CHECK(c.sensitivity == 0.5 && c.axis_scale[4] == -1.0 && c.axis_map[1] == 2);
	CHECK(c.nkeymap == 2 && c.keymap[1].keys[0] == 29 && c.keymap[1].keys[1] == 46);
	CHECK(!strcmp(c.lcd_label[2], "Measure"));
	CHECK(c.enable_spnav == 0);
	/* the shipped example config must parse cleanly */
	tdx_config_defaults(&c);
	CHECK(tdx_config_load(&c, "packaging/tdxd.conf") == 0);
	CHECK(c.enable_uinput == 1 && c.enable_spnav == 1 && c.lcd_brightness == 80);
	CHECK(c.nkeymap == 4 && !strcmp(c.lcd_label[9], "Paste"));
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	const struct tdx_keymap *km = tdx_keymap_for(&c, spp, find_btn(spp, "V3DK_ESC"));
	CHECK(km && km->keys[0] == 1);
}

int main(void)
{
	test_device_table();
	test_axes();
	test_buttons();
	test_lcd();
	test_config();
	printf("%d/%d checks passed\n", checks - fails, checks);
	return fails ? 1 : 0;
}
