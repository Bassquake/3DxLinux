/* Host-side tests for the hardware-independent parts of tdxd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "devices.h"
#include "deflate.h"
#include "lcd.h"
#include "lcd_ent.h"
#include "lcd_giotto.h"
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

static const char *vec_dir;

static void dump_vector(const char *name, const uint8_t *raw, size_t n)
{
	static uint8_t comp[TDX_DEFLATE_BOUND(1600000)];
	long c = tdx_deflate_fixed(raw, n, comp, sizeof comp);
	CHECK(c > 0 && (size_t)c <= TDX_DEFLATE_BOUND(n));
	if (!vec_dir || c <= 0)
		return;
	char p[512];
	snprintf(p, sizeof p, "%s/%s.raw", vec_dir, name);
	FILE *f = fopen(p, "wb");
	if (f) { fwrite(raw, 1, n, f); fclose(f); }
	snprintf(p, sizeof p, "%s/%s.def", vec_dir, name);
	f = fopen(p, "wb");
	if (f) { fwrite(comp, 1, (size_t)c, f); fclose(f); }
}

static void test_enterprise_lcd(void)
{
	const struct tdx_device_def *ent = tdx_device_lookup(0x256F, 0xC633);
	CHECK(ent && ent->lcd == TDX_LCD_ENT640);
	CHECK(TDX_ENT_RAW == 0x2ee00);

	static uint32_t px[TDX_ENT_PIXELS];
	static uint16_t a[TDX_ENT_PIXELS], b[TDX_ENT_PIXELS];
	px[0] = 0xFF0000; px[1] = 0x00FF00; px[2] = 0x0000FF; px[3] = 0x000000;
	px[TDX_ENT_PIXELS - 1] = 0xFFFFFF;
	tdx_ent_pack(px, a, 0);
	CHECK(a[0] == 0xF800 && a[1] == 0x07E0 && a[2] == 0x001F);
	CHECK(a[3] == 0x0821 && a[4] == 0x0821);   /* black never sent as 0 */
	CHECK(a[TDX_ENT_PIXELS - 1] == 0xFFFF);
	tdx_ent_pack(px, b, 1);                      /* swap_rb */
	CHECK(b[0] == 0x001F && b[2] == 0xF800);

	memcpy(b, a, sizeof b);
	b[10] = 0x1234;
	CHECK(tdx_ent_delta(b, a) == 1 && b[10] == 0x1234 && b[0] == 0 && b[11] == 0);

	uint8_t pkt[TDX_ENT_PKT];
	tdx_ent_header(pkt, TDX_ENT_OP_FULL, 0x01020304);
	const uint8_t full[16] = {0x11, 0x0F, 0x04, 0x03, 0x02, 0x01, 0, 0,
				  0, 0, 0x00, 0x3F, 0x01, 0xEF, 0x00, 0x0F};
	CHECK(!memcmp(pkt, full, 16) && pkt[16] == 0 && pkt[511] == 0);
	tdx_ent_header(pkt, TDX_ENT_OP_DELTA, 5);
	const uint8_t delta[16] = {0x18, 0x0F, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x0F};
	CHECK(!memcmp(pkt, delta, 16));
	tdx_ent_header(pkt, TDX_ENT_OP_FIRST, 5);
	CHECK(pkt[0] == 0x15 && pkt[11] == 0x3F && pkt[13] == 0xEF);

	/* full encode: header length must equal payload size */
	static uint8_t comp[TDX_DEFLATE_BOUND(TDX_ENT_RAW)];
	long n = tdx_ent_encode(a, TDX_ENT_OP_FULL, pkt, comp, sizeof comp);
	CHECK(n > 0 && n < 4000); /* mostly black -> highly compressible */
	CHECK(n > 0 && (uint32_t)(pkt[2] | pkt[3] << 8 | pkt[4] << 16 | (uint32_t)pkt[5] << 24) == (uint32_t)n);
}

static void test_deflate_vectors(void)
{
	static uint8_t buf[TDX_ENT_RAW];
	dump_vector("empty", buf, 0);
	const uint8_t one[1] = {0x42};
	dump_vector("one_byte", one, 1);
	const char *txt = "abcabcabcabcabcabcabc the quick brown fox abcabcabc jumps abcabc";
	dump_vector("text", (const uint8_t *)txt, strlen(txt));
	memset(buf, 0, sizeof buf);
	dump_vector("zeros_192000", buf, sizeof buf);
	uint32_t x = 12345;
	for (size_t i = 0; i < sizeof buf; i++) {
		x = x * 1103515245u + 12345u;
		buf[i] = (uint8_t)(x >> 16);
	}
	dump_vector("random_192000", buf, sizeof buf); /* worst case: all literals */
	for (size_t i = 0; i < sizeof buf; i++)
		buf[i] = (uint8_t)((i / 2) % 640 < 320 ? (i & 1 ? 0xF8 : 0x00) : (i % 7) * 31);
	dump_vector("stripes_192000", buf, sizeof buf);
	/* a rendered Enterprise page: gradient + bars */
	static uint32_t px[TDX_ENT_PIXELS];
	static uint16_t p565[TDX_ENT_PIXELS];
	for (int y = 0; y < TDX_ENT_H; y++)
		for (int xx = 0; xx < TDX_ENT_W; xx++)
			px[y * TDX_ENT_W + xx] = ((uint32_t)(xx * 255 / 639) << 16) | ((uint32_t)(y * 255 / 149) << 8) |
						 (uint32_t)((xx / 80) & 1 ? 0xC0 : 0x20);
	tdx_ent_pack(px, p565, 0);
	for (size_t i = 0; i < TDX_ENT_PIXELS; i++) {
		buf[2 * i] = (uint8_t)p565[i];
		buf[2 * i + 1] = (uint8_t)(p565[i] >> 8);
	}
	dump_vector("ent_page_192000", buf, sizeof buf);
}

static void test_giotto_lcd(void)
{
	const struct tdx_device_def *gio = tdx_device_lookup(0x256F, 0xC63C);
	CHECK(gio && gio->lcd == TDX_LCD_GIO1280);
	CHECK(TDX_GIO_RAW == 1597440);

	/* rotation: out[((1279 - x) * 416 + y) * 3] = B,G,R of pixel (x, y) */
	static uint32_t px[TDX_GIO_PIXELS];
	static uint8_t raw[TDX_GIO_RAW];
	memset(px, 0, sizeof px);
	px[0] = 0x112233;                              /* (0,0)    */
	px[5 * TDX_GIO_W + 7] = 0xAABBCC;              /* (7,5)    */
	px[TDX_GIO_PIXELS - 1] = 0x445566;             /* (1279,415) */
	tdx_gio_pack(px, raw, 0);
	size_t o00 = ((size_t)1279 * 416 + 0) * 3, o75 = ((size_t)(1279 - 7) * 416 + 5) * 3;
	size_t oend = ((size_t)0 * 416 + 415) * 3;
	CHECK(raw[o00] == 0x33 && raw[o00 + 1] == 0x22 && raw[o00 + 2] == 0x11);
	CHECK(raw[o75] == 0xCC && raw[o75 + 1] == 0xBB && raw[o75 + 2] == 0xAA);
	CHECK(raw[oend] == 0x66 && raw[oend + 2] == 0x44);
	tdx_gio_pack(px, raw, 1);
	CHECK(raw[o00] == 0x11 && raw[o00 + 2] == 0x33);

	/* command packets: 64 bytes, 55 AA trailer at 22..23 */
	uint8_t p[TDX_GIO_PKT];
	tdx_gio_cmd_write(p, 1, 0x00123456);
	const uint8_t w[8] = {0x20, 0, 1, 1, 0x56, 0x34, 0x12, 0};
	CHECK(!memcmp(p, w, 8) && p[22] == 0x55 && p[23] == 0xAA && p[21] == 0 && p[63] == 0);
	tdx_gio_cmd_show(p, 2);
	CHECK(p[0] == 0x24 && p[1] == 2 && p[2] == 0 && p[22] == 0x55);
	tdx_gio_cmd_copy(p, 1, 3);
	CHECK(p[0] == 0x22 && p[1] == 1 && p[2] == 3);

	/* frame sequence from FUN_180048fb0 / FUN_18004aee0: A=1, B=0, C=3 */
	int a = 1, b = 0;
	uint8_t pk[4][TDX_GIO_PKT];
	tdx_gio_plan_frame(&a, &b, 3, 100, pk);
	CHECK(pk[0][0] == 0x20 && pk[0][2] == 1 && pk[0][4] == 100);
	CHECK(pk[1][0] == 0x24 && pk[1][1] == 1);
	CHECK(pk[2][0] == 0x22 && pk[2][1] == 1 && pk[2][2] == 0);
	CHECK(pk[3][0] == 0x22 && pk[3][1] == 1 && pk[3][2] == 3);
	CHECK(a == 0 && b == 1);
	tdx_gio_plan_frame(&a, &b, 3, 100, pk); /* second frame alternates buffers */
	CHECK(pk[0][2] == 0 && pk[1][1] == 0 && pk[2][1] == 0 && pk[2][2] == 1 && pk[3][2] == 3);
	CHECK(a == 1 && b == 0);

	/* a rendered Giotto page must compress into Windows' 0x1ad080-byte buffer */
	struct tdx_config cfg;
	tdx_config_defaults(&cfg);
	struct tdx_fb fb = {TDX_GIO_W, TDX_GIO_H, px};
	tdx_render_ent_page(&fb, &cfg, "Giotto", 0, 1);
	tdx_gio_pack(px, raw, 0);
	dump_vector("giotto_page_1597440", raw, sizeof raw);
	static uint8_t comp[TDX_DEFLATE_BOUND(TDX_GIO_RAW)];
	long n = tdx_deflate_fixed(raw, sizeof raw, comp, sizeof comp);
	CHECK(n > 0 && n < TDX_GIO_COMP_MAX);
}

static void test_giotto_slide(void)
{
	/* 0x23 byte layout from FUN_180048750 */
	uint8_t p[TDX_GIO_PKT];
	tdx_gio_cmd_copy_rect(p, 2, 1, 60, 0, 60, 320, 341, 960);
	const uint8_t exp[24] = {0x23, 2, 1, 0, 0, 0, 0, 0,
				 60, 0, 0, 0, 60, 0, 0x40, 0x01,
				 0x55, 0x01, 0xC0, 0x03, 0, 0, 0x55, 0xAA};
	CHECK(!memcmp(p, exp, 24) && p[63] == 0);

	struct tdx_gio_state s = {.a = 1, .b = 0, .c = 3, .d = 2, .page_in_c = -1, .page_in_d = -1};
	struct tdx_gio_step st[8];
	int n;

	/* x = 0: D <- page 0, full-width copy D->A, show A */
	n = tdx_gio_plan_slide(&s, 0, 3, st, 8);
	CHECK(n == 3);
	CHECK(st[0].kind == TDX_GIO_STEP_UPLOAD && st[0].buf == 2 && st[0].page == 0);
	CHECK(st[1].kind == TDX_GIO_STEP_CMD && st[1].pkt[0] == 0x23 && st[1].pkt[1] == 2 &&
	      st[1].pkt[2] == 1 && st[1].pkt[18] == 0x00 && st[1].pkt[19] == 0x05); /* w 1280 */
	CHECK(st[2].pkt[0] == 0x24 && st[2].pkt[1] == 1 && s.a == 0 && s.b == 1);

	/* x = 320: C <- page 1; two copies into the new back buffer (0); show 0 */
	n = tdx_gio_plan_slide(&s, 320, 3, st, 8);
	CHECK(n == 4);
	CHECK(st[0].kind == TDX_GIO_STEP_UPLOAD && st[0].buf == 3 && st[0].page == 1);
	CHECK(st[1].pkt[1] == 2 && st[1].pkt[2] == 0 && st[1].pkt[14] == 0x40 && st[1].pkt[15] == 1 &&
	      st[1].pkt[18] == 0xC0 && st[1].pkt[19] == 3);              /* dmx 320, w 960 */
	CHECK(st[2].pkt[1] == 3 && st[2].pkt[10] == 0xC0 && st[2].pkt[11] == 3 &&
	      st[2].pkt[14] == 0 && st[2].pkt[18] == 0x40 && st[2].pkt[19] == 1); /* smx 960, w 320 */
	CHECK(st[3].pkt[0] == 0x24 && st[3].pkt[1] == 0 && s.b == 0 && s.a == 1);

	/* x = 640: pages already on the device -> no uploads */
	n = tdx_gio_plan_slide(&s, 640, 3, st, 8);
	CHECK(n == 3 && st[0].kind == TDX_GIO_STEP_CMD && st[1].kind == TDX_GIO_STEP_CMD);

	/* x = 1280: page 1 at offset 0 -> D re-uploaded with page 1, one full copy */
	n = tdx_gio_plan_slide(&s, 1280, 3, st, 8);
	CHECK(n == 3 && st[0].kind == TDX_GIO_STEP_UPLOAD && st[0].buf == 2 && st[0].page == 1);

	/* last page (and anything beyond, clamped): whole page from C */
	n = tdx_gio_plan_slide(&s, 99999, 3, st, 8);
	CHECK(n == 3 && st[0].kind == TDX_GIO_STEP_UPLOAD && st[0].buf == 3 && st[0].page == 2);
	CHECK(st[1].pkt[1] == 3 && st[1].pkt[10] == 0 && st[1].pkt[14] == 0 && st[1].pkt[19] == 5);
	CHECK(tdx_gio_plan_slide(&s, 0, 0, st, 8) == -1 && tdx_gio_plan_slide(&s, 0, 5, st, 8) == -1);
}

/*
 * Minimal Giotto device model: 4 frame buffers in the panel's native layout
 * (pixel index = mirrored_x * 416 + y), executing 0x22 / 0x23 / 0x24 and
 * uploads (0x20, modelled without compression). Checks that the planner's
 * mirrored-x rectangles produce the intended on-screen composition.
 */
static uint32_t gdev[4][TDX_GIO_PIXELS];
static int gshown = -1;

static uint32_t screen_px(int buf, int x, int y)
{
	return gdev[buf][(size_t)(TDX_GIO_W - 1 - x) * TDX_GIO_H + y];
}

static void gdev_upload(int buf, const uint32_t *img)
{
	for (int y = 0; y < TDX_GIO_H; y++)
		for (int x = 0; x < TDX_GIO_W; x++)
			gdev[buf][(size_t)(TDX_GIO_W - 1 - x) * TDX_GIO_H + y] = img[y * TDX_GIO_W + x];
}

static uint16_t u16at(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void gdev_exec(const uint8_t *pkt)
{
	if (pkt[0] == 0x22)
		memcpy(gdev[pkt[2]], gdev[pkt[1]], sizeof gdev[0]);
	else if (pkt[0] == 0x24)
		gshown = pkt[1];
	else if (pkt[0] == 0x23) {
		int src = pkt[1], dst = pkt[2];
		int sy = u16at(pkt + 8), smx = u16at(pkt + 10), dy = u16at(pkt + 12), dmx = u16at(pkt + 14);
		int h = u16at(pkt + 16), w = u16at(pkt + 18);
		for (int c = 0; c < w; c++)
			for (int r = 0; r < h; r++)
				gdev[dst][(size_t)(dmx + c) * TDX_GIO_H + dy + r] =
					gdev[src][(size_t)(smx + c) * TDX_GIO_H + sy + r];
	}
}

static void test_giotto_slide_model(void)
{
	static uint32_t pg[3][TDX_GIO_PIXELS];
	for (int p = 0; p < 3; p++) /* each pixel encodes (page, x, y) */
		for (int y = 0; y < TDX_GIO_H; y++)
			for (int x = 0; x < TDX_GIO_W; x++)
				pg[p][y * TDX_GIO_W + x] = (uint32_t)(p << 22 | x << 9 | y);

	/* static page 1 via the 0x20/0x24/0x22/0x22 sequence (model: upload to A) */
	int a = 1, b = 0;
	uint8_t pk[4][TDX_GIO_PKT];
	tdx_gio_plan_frame(&a, &b, 3, 1, pk);
	gdev_upload(pk[0][2], pg[0]);
	for (int i = 1; i < 4; i++)
		gdev_exec(pk[i]);
	CHECK(gshown == b && screen_px(gshown, 5, 7) == pg[0][7 * TDX_GIO_W + 5]);

	struct tdx_gio_state s = {.a = a, .b = b, .c = 3, .d = 2, .page_in_c = -1, .page_in_d = -1};
	struct tdx_gio_step st[8];
	const int xs[] = {320, 1, 1279, 1280, 1900, 2560};
	for (size_t t = 0; t < sizeof xs / sizeof xs[0]; t++) {
		int n = tdx_gio_plan_slide(&s, xs[t], 3, st, 8);
		for (int i = 0; i < n; i++) {
			if (st[i].kind == TDX_GIO_STEP_UPLOAD)
				gdev_upload(st[i].buf, pg[st[i].page]);
			else
				gdev_exec(st[i].pkt);
		}
		int page = xs[t] / TDX_GIO_W, off = xs[t] % TDX_GIO_W, bad = 0;
		for (int y = 0; y < TDX_GIO_H && !bad; y += 3)
			for (int x = 0; x < TDX_GIO_W && !bad; x += 7) {
				uint32_t want;
				if (y < TDX_GIO_SLIDE_Y || y >= TDX_GIO_SLIDE_Y + TDX_GIO_SLIDE_H)
					want = pg[0][y * TDX_GIO_W + x];       /* outside the band: unchanged */
				else if (page == 2)
					want = pg[2][y * TDX_GIO_W + x];       /* last page, whole */
				else if (x < TDX_GIO_W - off)
					want = pg[page][y * TDX_GIO_W + x + off];
				else
					want = pg[page + 1][y * TDX_GIO_W + x - (TDX_GIO_W - off)];
				bad = screen_px(gshown, x, y) != want;
			}
		CHECK(!bad);
		CHECK(gshown == s.b);
	}
}

static void test_giotto_move(void)
{
	struct tdx_gio_state s = {.a = 0, .b = 1, .c = 3, .d = 2, .page_in_c = -1, .page_in_d = -1};
	struct tdx_gio_step st[4];
	int n;

	/* y = 0: moving image at row 60, nothing uncovered */
	n = tdx_gio_plan_move(&s, 0, 0, st, 4);
	CHECK(n == 1 && st[0].pkt[0] == 0x23 && st[0].pkt[1] == 2 && st[0].pkt[2] == 0);
	CHECK(u16at(st[0].pkt + 8) == 60 && u16at(st[0].pkt + 12) == 60 &&
	      u16at(st[0].pkt + 16) == 356 && u16at(st[0].pkt + 18) == 1280);

	/* y = 100: moving image at 160 (h 256), rows 0..99 restored from C */
	n = tdx_gio_plan_move(&s, 0, 100, st, 4);
	CHECK(n == 2 && u16at(st[0].pkt + 12) == 160 && u16at(st[0].pkt + 16) == 256);
	CHECK(st[1].pkt[1] == 3 && st[1].pkt[2] == 0 && u16at(st[1].pkt + 8) == 0 &&
	      u16at(st[1].pkt + 12) == 0 && u16at(st[1].pkt + 16) == 100);

	/* y = 400: moving image gone (356 - y <= 0), rows 100..399 restored */
	n = tdx_gio_plan_move(&s, 0, 400, st, 4);
	CHECK(n == 1 && st[0].pkt[1] == 3 && u16at(st[0].pkt + 8) == 100 && u16at(st[0].pkt + 16) == 300);

	/* y > 416: Windows skips the restore and keeps the old position */
	n = tdx_gio_plan_move(&s, 0, 500, st, 4);
	CHECK(n == 0 && s.move_y == 400);
	/* moving back up: no restore copy, position recorded */
	n = tdx_gio_plan_move(&s, 0, 50, st, 4);
	CHECK(n == 1 && st[0].pkt[1] == 2 && s.move_y == 50);
	CHECK(tdx_gio_plan_move(&s, 0, -1, st, 4) == -1);

	/* device model: present A after each step, check what is on screen */
	static uint32_t sheet[TDX_GIO_PIXELS], bg[TDX_GIO_PIXELS];
	for (int y = 0; y < TDX_GIO_H; y++)
		for (int x = 0; x < TDX_GIO_W; x++) {
			sheet[y * TDX_GIO_W + x] = (uint32_t)(1u << 22 | x << 9 | y);
			bg[y * TDX_GIO_W + x] = (uint32_t)(2u << 22 | x << 9 | y);
		}
	int a = 1, b = 0;
	uint8_t pk[4][TDX_GIO_PKT];
	tdx_gio_plan_frame(&a, &b, 3, 1, pk); /* static sheet first */
	gdev_upload(pk[0][2], sheet);
	for (int i = 1; i < 4; i++)
		gdev_exec(pk[i]);
	struct tdx_gio_state m = {.a = a, .b = b, .c = 3, .d = 2};
	gdev_upload(m.d, sheet);
	gdev_upload(m.c, bg);
	int bad = 0;
	for (int y = 40; y <= 416 && !bad; y += 40) {
		n = tdx_gio_plan_move(&m, 0, y, st, 4);
		for (int i = 0; i < n; i++)
			gdev_exec(st[i].pkt);
		gshown = m.a; /* present (tdxd addition) */
		for (int r = 0; r < TDX_GIO_H && !bad; r += 5)
			for (int x = 0; x < TDX_GIO_W && !bad; x += 11) {
				uint32_t got = screen_px(gshown, x, r);
				if (r < y)
					bad = got != bg[r * TDX_GIO_W + x];
				else if (r >= y + 60)
					bad = got != sheet[(r - y) * TDX_GIO_W + x];
			}
	}
	CHECK(!bad);
}

int main(int argc, char **argv)
{
	vec_dir = argc > 1 ? argv[1] : NULL;
	test_enterprise_lcd();
	test_giotto_lcd();
	test_giotto_slide();
	test_giotto_slide_model();
	test_giotto_move();
	test_deflate_vectors();
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
