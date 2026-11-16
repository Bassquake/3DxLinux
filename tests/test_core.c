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

static void test_rgb_leds(void)
{
	const struct tdx_device_def *giotto = tdx_device_lookup(0x256F, 0xC63C);
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	uint8_t r[40][8];
	struct tdx_config c;

	CHECK(tdx_has_rgb_leds(giotto) && !tdx_has_rgb_leds(spp));
	tdx_rgb_report(7, 0x123456, r[0]);
	const uint8_t exp[8] = {0x1A, 0x25, 0x07, 0x12, 0x34, 0x56, 0, 0};
	CHECK(!memcmp(r[0], exp, 8));

	tdx_config_defaults(&c);
	CHECK(tdx_rgb_plan(&c, giotto, r, 40) == 0); /* nothing configured: leave LEDs alone */
	CHECK(tdx_rgb_plan(&c, spp, r, 40) == 0);

	c.led = 0; /* off -> single 0x24 broadcast with black */
	CHECK(tdx_rgb_plan(&c, giotto, r, 40) == 1 && r[0][2] == 0x24 && !r[0][3] && !r[0][5]);

	c.led = 1; /* on, no colours -> white buttons (0x25) + white ring (0x26) */
	CHECK(tdx_rgb_plan(&c, giotto, r, 40) == 2);
	CHECK(r[0][2] == 0x25 && r[0][3] == 0xFF && r[1][2] == 0x26 && r[1][5] == 0xFF);

	c.led = -1; /* uniform button colour only */
	c.led_button_color = 0x00FF00;
	CHECK(tdx_rgb_plan(&c, giotto, r, 40) == 1 && r[0][2] == 0x25 && r[0][4] == 0xFF);

	/* one button differs -> 30 individual reports; LED via Base.xml LEDNumber */
	int fit = find_btn(giotto, "V3DK_FIT");
	c.nledcolor = 1;
	snprintf(c.ledcolor[0].v3dk, sizeof c.ledcolor[0].v3dk, "V3DK_FIT");
	c.ledcolor[0].rgb = 0xFF0000;
	c.led_ring_color = 0x0000FF;
	int n = tdx_rgb_plan(&c, giotto, r, 40);
	CHECK(n == 31);
	int led = giotto->buttons[fit].led;
	CHECK(led >= 0 && r[led][2] == led && r[led][3] == 0xFF && r[led][4] == 0);
	CHECK(r[30][2] == 0x26 && r[30][5] == 0xFF);

	/* individual ring segment by LED_n */
	snprintf(c.ledcolor[0].v3dk, sizeof c.ledcolor[0].v3dk, "LED_31");
	n = tdx_rgb_plan(&c, giotto, r, 40);
	CHECK(n == 1 + 6 && r[0][2] == 0x25 && r[2][2] == 31 && r[2][3] == 0xFF && r[1][5] == 0xFF);

	/* config parsing */
	const char *p = "test_rgb.conf";
	FILE *f = fopen(p, "w");
	fputs("led_button_color = #102030\nled_ring_color = 0xA0B0C0\nled_color_V3DK_FIT = ff8000\n"
	      "led_color_LED_32 = 00ff00\n", f);
	fclose(f);
	tdx_config_defaults(&c);
	CHECK(tdx_config_load(&c, p) == 0);
	remove(p);
	CHECK(c.led_button_color == 0x102030 && c.led_ring_color == 0xA0B0C0 && c.nledcolor == 2);
	CHECK(!strcmp(c.ledcolor[0].v3dk, "V3DK_FIT") && c.ledcolor[0].rgb == 0xFF8000);
}

static void test_lcd_intensity(void)
{
	const struct tdx_device_def *ent = tdx_device_lookup(0x256F, 0xC633);
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	uint8_t r[2] = {0};
	CHECK(tdx_lcd_intensity_report(ent, 60, r) == 2 && r[0] == 0x11 && r[1] == 60);
	CHECK(tdx_lcd_intensity_report(ent, 250, r) == 2 && r[1] == 100);
	CHECK(tdx_lcd_intensity_report(ent, -1, r) == 0); /* lcd_brightness unset */
	CHECK(tdx_lcd_intensity_report(spp, 60, r) == 0); /* SPP uses the USB vendor request */
}

int main(void)
{
	test_lcd_intensity();
	test_device_table();
	test_axes();
	test_buttons();
	test_lcd();
	test_config();
	test_rgb_leds();
	printf("%d/%d checks passed\n", checks - fails, checks);
	return fails ? 1 : 0;
}
