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
	tdx_render_ent_page(&fb, &cfg, "Giotto");
	tdx_gio_pack(px, raw, 0);
	dump_vector("giotto_page_1597440", raw, sizeof raw);
	static uint8_t comp[TDX_DEFLATE_BOUND(TDX_GIO_RAW)];
	long n = tdx_deflate_fixed(raw, sizeof raw, comp, sizeof comp);
	CHECK(n > 0 && n < TDX_GIO_COMP_MAX);
}

int main(int argc, char **argv)
{
	vec_dir = argc > 1 ? argv[1] : NULL;
	test_enterprise_lcd();
	test_giotto_lcd();
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
