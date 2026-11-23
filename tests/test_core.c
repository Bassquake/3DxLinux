/* Host-side tests for the hardware-independent parts of tdxd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>

#include "config.h"
#include "control.h"
#include "profile.h"
#include "lcdmgr_db.h"
#include "lcdmgr_core.h"
#include "devices.h"
#include "deflate.h"
#include "lcd.h"
#include "lcd_ent.h"
#include "lcd_giotto.h"
#include "process.h"
#include "receiver.h"
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
	CHECK(c.sensitivity == 0.5 && c.axis_invert[4] == 1 && c.axis_scale[4] == 1.0 && c.axis_map[1] == 2);
	CHECK(c.nkeymap == 2 && c.keymap[1].keys[0] == 29 && c.keymap[1].keys[1] == 46);
	CHECK(!strcmp(c.lcd_label[2], "Measure"));
	CHECK(c.enable_spnav == 0);
	/* the shipped example config must parse cleanly */
	tdx_config_defaults(&c);
	CHECK(tdx_config_load(&c, "packaging/tdxd.conf") == 0);
	CHECK(c.enable_uinput == 1 && c.enable_spnav == 1 && c.lcd_brightness == 80);
	/* ESC ALT SHIFT CTRL + keys 1-12 with 3DxWare's defaults (SharedCfg.xml) */
	CHECK(c.nkeymap == 16 && !strcmp(c.lcd_label[0], "New") && !strcmp(c.lcd_label[9], "Undo"));
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	const struct tdx_keymap *km = tdx_keymap_for(&c, spp, find_btn(spp, "V3DK_ESC"));
	CHECK(km && km->keys[0] == 1);
	km = tdx_keymap_for(&c, spp, find_btn(spp, "V3DK_4"));    /* Copy */
	CHECK(km && km->keys[0] == KEY_LEFTCTRL && km->keys[1] == KEY_C && !km->seq);
	km = tdx_keymap_for(&c, spp, find_btn(spp, "V3DK_9"));    /* switch application */
	CHECK(km && km->keys[0] == KEY_LEFTALT && km->keys[1] == KEY_TAB);
	CHECK(tdx_action_for(&c, spp, find_btn(spp, "V3DK_MENU")) == TDX_ACT_SETTINGS);
	CHECK(!strcmp(tdx_action_name(TDX_ACT_SETTINGS), "settings"));
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

/* model: execute a 0x21 rectangle write using the real packed payload bytes */
static void gdev_write_rect(const uint8_t *pkt, const uint8_t *packed)
{
	int buf = pkt[2], y = u16at(pkt + 12), mx = u16at(pkt + 14), h = u16at(pkt + 16),
	    w = u16at(pkt + 18);
	for (int c = 0; c < w; c++)
		for (int r = 0; r < h; r++) {
			const uint8_t *o = packed + ((size_t)c * h + r) * 3; /* B,G,R */
			gdev[buf][(size_t)(mx + c) * TDX_GIO_H + y + r] =
				(uint32_t)o[2] << 16 | (uint32_t)o[1] << 8 | o[0];
		}
}

static void test_giotto_rect(void)
{
	/* header bytes (FUN_180048630) */
	uint8_t p[TDX_GIO_PKT];
	tdx_gio_cmd_write_rect(p, 1, 0x1234, 60, 1000, 341, 200);
	const uint8_t exp[24] = {0x21, 0, 1, 1, 0x34, 0x12, 0, 0, 0, 0, 0, 0,
				 60, 0, 0xE8, 0x03, 0x55, 0x01, 0xC8, 0x00, 0, 0, 0x55, 0xAA};
	CHECK(!memcmp(p, exp, 24) && p[63] == 0);
	CHECK(tdx_gio_comp_max(1280, 416) == 0x1ad080); /* matches the full-frame buffer */

	/* rect-local rotation */
	static uint32_t img[TDX_GIO_PIXELS], img2[TDX_GIO_PIXELS];
	static uint8_t packed[TDX_GIO_RAW];
	uint32_t r = 7;
	for (size_t i = 0; i < TDX_GIO_PIXELS; i++) {
		r = r * 1103515245u + 12345u;
		img[i] = (r >> 8) & 0xFFFFFF;
		r = r * 1103515245u + 12345u;
		img2[i] = (r >> 8) & 0xFFFFFF;
	}
	tdx_gio_pack_rect(img2, 100, 20, 30, 10, packed, 0);
	uint32_t px0 = img2[20 * TDX_GIO_W + 100];  /* (100,20) -> local col 29, row 0 */
	CHECK(packed[(29 * 10 + 0) * 3] == (uint8_t)px0 && packed[(29 * 10 + 0) * 3 + 2] == (uint8_t)(px0 >> 16));
	uint32_t px1 = img2[29 * TDX_GIO_W + 129];  /* (129,29) -> local col 0, row 9 */
	CHECK(packed[(0 * 10 + 9) * 3 + 1] == (uint8_t)(px1 >> 8));

	/* planner */
	struct tdx_gio_state s = {.a = 1, .b = 0, .c = 3, .d = 2, .page_in_c = 2, .page_in_d = 0};
	uint8_t pk[4][TDX_GIO_PKT];
	CHECK(tdx_gio_plan_rect(&s, 0, 0, 0, 10, 5, pk) == -1);
	CHECK(tdx_gio_plan_rect(&s, 1200, 0, 100, 10, 5, pk) == -1);
	CHECK(tdx_gio_plan_rect(&s, 100, 20, 30, 10, 5, pk) == 4);
	CHECK(pk[0][0] == 0x21 && pk[0][2] == 1 && u16at(pk[0] + 14) == 1280 - 30 - 100);
	CHECK(pk[1][0] == 0x24 && pk[1][1] == 1);
	CHECK(pk[2][0] == 0x23 && pk[2][1] == 1 && pk[2][2] == 0 && u16at(pk[2] + 8) == 20 &&
	      u16at(pk[2] + 10) == 1150 && u16at(pk[2] + 12) == 20 && u16at(pk[2] + 14) == 1150 &&
	      u16at(pk[2] + 16) == 10 && u16at(pk[2] + 18) == 30);
	CHECK(pk[3][0] == 0x22 && pk[3][1] == 1 && pk[3][2] == 3);
	CHECK(s.a == 0 && s.b == 1 && s.page_in_c == -1 && s.page_in_d == 0);

	/* device model: static frame, a slide step (desyncs A/B), sync, title-bar rect */
	int a = 1, b = 0;
	tdx_gio_plan_frame(&a, &b, 3, 1, pk);
	gdev_upload(pk[0][2], img);
	for (int i = 1; i < 4; i++)
		gdev_exec(pk[i]);
	struct tdx_gio_state m = {.a = a, .b = b, .c = 3, .d = 2, .page_in_c = -1, .page_in_d = -1};
	struct tdx_gio_step st[8];
	int n = tdx_gio_plan_slide(&m, 0, 1, st, 8); /* single page: full copy from C */
	for (int i = 0; i < n; i++) {
		if (st[i].kind == TDX_GIO_STEP_UPLOAD)
			gdev_upload(st[i].buf, img);
		else
			gdev_exec(st[i].pkt);
	}
	uint8_t sync[TDX_GIO_PKT];
	tdx_gio_cmd_copy(sync, (uint8_t)m.b, (uint8_t)m.a); /* tdx_giolcd_sync_back */
	gdev_exec(sync);
	const int rx = 0, ry = 0, rw = TDX_GIO_W, rh = TDX_GIO_SLIDE_Y; /* title bar */
	tdx_gio_pack_rect(img2, rx, ry, rw, rh, packed, 0);
	CHECK(tdx_gio_plan_rect(&m, rx, ry, rw, rh, 1, pk) == 4);
	gdev_write_rect(pk[0], packed);
	for (int i = 1; i < 4; i++)
		gdev_exec(pk[i]);
	int bad = 0;
	for (int y = 0; y < TDX_GIO_H && !bad; y += 3)
		for (int x = 0; x < TDX_GIO_W && !bad; x += 5) {
			const uint32_t *src = (y >= ry && y < ry + rh) ? img2 : img;
			bad = screen_px(gshown, x, y) != src[y * TDX_GIO_W + x] ||
			      screen_px(m.a, x, y) != src[y * TDX_GIO_W + x] ||  /* back in sync */
			      screen_px(m.c, x, y) != src[y * TDX_GIO_W + x];    /* C refreshed */
		}
	CHECK(!bad && gshown == m.b);

	/* an off-centre rectangle lands where it should */
	tdx_gio_pack_rect(img, 100, 200, 300, 50, packed, 0);
	CHECK(tdx_gio_plan_rect(&m, 100, 200, 300, 50, 1, pk) == 4);
	gdev_write_rect(pk[0], packed);
	for (int i = 1; i < 4; i++)
		gdev_exec(pk[i]);
	CHECK(screen_px(gshown, 100, 200) == img[200 * TDX_GIO_W + 100] &&
	      screen_px(gshown, 399, 249) == img[249 * TDX_GIO_W + 399] &&
	      screen_px(gshown, 99, 200) == img[200 * TDX_GIO_W + 99] &&     /* unchanged anyway */
	      screen_px(gshown, 400, 30) == img2[30 * TDX_GIO_W + 400]);     /* title bar kept */
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* write a bottom-up BMP of @img (w x h, 0x00RRGGBB) with @bpp 24 or 32 */
static int write_bmp(const char *path, const uint32_t *img, int w, int h, int bpp)
{
	int stride = (w * bpp + 31) / 32 * 4;
	uint8_t hdr[54] = {'B', 'M'};
	put_le32(hdr + 2, (uint32_t)(54 + stride * h));
	put_le32(hdr + 10, 54);
	put_le32(hdr + 14, 40);
	put_le32(hdr + 18, (uint32_t)w);
	put_le32(hdr + 22, (uint32_t)h);
	hdr[26] = 1;
	hdr[28] = (uint8_t)bpp;
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	fwrite(hdr, 1, sizeof hdr, f);
	uint8_t *row = calloc(1, (size_t)stride);
	for (int y = h - 1; y >= 0; y--) { /* bottom-up */
		for (int x = 0; x < w; x++) {
			uint32_t p = img[(size_t)y * w + x];
			uint8_t *o = row + x * (bpp / 8);
			o[0] = (uint8_t)p; o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)(p >> 16);
		}
		fwrite(row, 1, (size_t)stride, f);
	}
	free(row);
	fclose(f);
	return 0;
}

static void test_giotto_bmp(void)
{
	/* header bytes (FUN_18004a8a0) */
	uint8_t p[TDX_GIO_PKT];
	tdx_gio_cmd_bmp_header(p, 0x00ABCDEF, 60, 1000, 341, 200);
	const uint8_t exp[16] = {0x13, 0x0F, 0xEF, 0xCD, 0xAB, 0x00, 0x00, 60, 0,
				 0xE8, 0x03, 0x55, 0x01, 0xC8, 0x00, 0x0F};
	CHECK(!memcmp(p, exp, 16) && p[16] == 0 && p[63] == 0);

	/* control file "(x, y), (w, h)"; unmatched fields keep defaults */
	const char *cp = "test_ctrl.txt";
	FILE *f = fopen(cp, "w");
	fputs("(10, 20), (300, 40)\n", f);
	fclose(f);
	int x = 0, y = 0, w = 1280, h = 416;
	tdx_gio_bmp_control(cp, &x, &y, &w, &h);
	CHECK(x == 10 && y == 20 && w == 300 && h == 40);
	f = fopen(cp, "w");
	fputs("(5, 6)\n", f);
	fclose(f);
	x = 0, y = 0, w = 1280, h = 416;
	tdx_gio_bmp_control(cp, &x, &y, &w, &h);
	CHECK(x == 5 && y == 6 && w == 1280 && h == 416);
	remove(cp);
	tdx_gio_bmp_control("does_not_exist.txt", &x, &y, &w, &h);
	CHECK(x == 5);

	/* 24 bpp bottom-up BMP: the 0x13 converter must equal the 0x21 rect packer */
	static uint32_t img[TDX_GIO_PIXELS];
	uint32_t r = 99;
	for (size_t i = 0; i < TDX_GIO_PIXELS; i++) {
		r = r * 1103515245u + 12345u;
		img[i] = (r >> 8) & 0xFFFFFF;
	}
	const char *bp = "test_sp4LCD.bmp";
	CHECK(write_bmp(bp, img, TDX_GIO_W, TDX_GIO_H, 24) == 0);
	struct tdx_bmp bmp;
	CHECK(tdx_bmp_load(bp, &bmp) == 0 && bmp.width == 1280 && bmp.height == 416 && bmp.bpp == 24);
	static uint8_t a[TDX_GIO_W * TDX_GIO_H * 4], b[TDX_GIO_RAW];
	long n = tdx_gio_bmp_convert(&bmp, 0, 0, TDX_GIO_W, TDX_GIO_H, a, sizeof a);
	tdx_gio_pack_rect(img, 0, 0, TDX_GIO_W, TDX_GIO_H, b, 0);
	CHECK(n == TDX_GIO_RAW && !memcmp(a, b, TDX_GIO_RAW));
	tdx_gio_pack(img, b, 0); /* and the full-frame 0x20 packer */
	CHECK(!memcmp(a, b, TDX_GIO_RAW));
	n = tdx_gio_bmp_convert(&bmp, 100, 200, 300, 50, a, sizeof a);
	tdx_gio_pack_rect(img, 100, 200, 300, 50, b, 0);
	CHECK(n == 300 * 50 * 3 && !memcmp(a, b, (size_t)n));
	dump_vector("giotto_bmp_rect", a, (size_t)n);
	tdx_bmp_free(&bmp);

	/* 32 bpp: payload is 4*w*h with a zero tail (and the 3-byte source step) */
	CHECK(write_bmp(bp, img, TDX_GIO_W, TDX_GIO_H, 32) == 0);
	CHECK(tdx_bmp_load(bp, &bmp) == 0 && bmp.bpp == 32);
	n = tdx_gio_bmp_convert(&bmp, 0, 0, 64, 8, a, sizeof a);
	int tail0 = 1;
	for (long i = 64 * 8 * 3; i < n; i++)
		tail0 &= a[i] == 0;
	CHECK(n == 64 * 8 * 4 && tail0);
	CHECK(a[((63 * 8) + 0) * 3] == bmp.bits[(size_t)415 * bmp.stride]); /* (0,0): first source byte */
	tdx_bmp_free(&bmp);
	remove(bp);
	CHECK(tdx_bmp_load("does_not_exist.bmp", &bmp) < 0);
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void test_rezero(void)
{
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	const struct tdx_device_def *ent = tdx_device_lookup(0x256F, 0xC633);
	uint8_t r[2] = {0xAA, 0xAA};
	CHECK(tdx_rezero_report(spp, 0x0300, r) == 2 && r[0] == 0x07 && r[1] == 0x00);
	CHECK(tdx_rezero_report(spp, 0x0200, r) == 2);
	CHECK(tdx_rezero_report(spp, 0x01FF, r) == 0); /* UdDevRezero: version >= 0x200 */
	CHECK(tdx_rezero_report(spp, 0, r) == 2);      /* unknown version: send */
	CHECK(tdx_rezero_report(ent, 0x0441, r) == 0); /* Enterprise needs > 0x441 */
	CHECK(tdx_rezero_report(ent, 0x0442, r) == 2);
}

static void test_profiles(void)
{
	/* base config and two profiles in a scratch directory */
	const char *base = "test_base.conf", *dir = "test_profiles.d";
	mkdir(dir, 0755);
	write_file(base, "sensitivity = 1.5\ninvert_ry = yes\nscale_tz = 0.5\n"
			 "key_V3DK_ESC = KEY_ESC\nlcd_label_1 = Base\n");
	write_file("test_profiles.d/zeta.conf",
		   "match = blender, blender-*   # Blender builds\n"
		   "invert_ry = no\nsensitivity = 2\nkey_V3DK_ESC = none\nkey_V3DK_FIT = KEY_F\n"
		   "spnav_socket = /tmp/ignored.sock\nlcd_label_1 = Prof\n");
	write_file("test_profiles.d/alpha.conf", "scale_rx = 0\n");
	write_file("test_profiles.d/notes.txt", "not a profile\n");
	write_file("test_profiles.d/.hidden.conf", "sensitivity = 9\n");

	static struct tdx_profiles ps;
	CHECK(tdx_profiles_scan(dir, &ps) == 0 && ps.n == 2);
	CHECK(!strcmp(ps.p[0].name, "alpha") && !strcmp(ps.p[1].name, "zeta"));
	CHECK(!strcmp(ps.p[0].match, "alpha") && !strcmp(ps.p[1].match, "blender, blender-*"));
	CHECK(tdx_profile_match(&ps, "blender") == 1);
	CHECK(tdx_profile_match(&ps, "Blender-3.6") == 1); /* case-insensitive glob */
	CHECK(tdx_profile_match(&ps, "ALPHA") == 0);
	CHECK(tdx_profile_match(&ps, "gimp") == -1 && tdx_profile_match(&ps, NULL) == -1);

	static struct tdx_config c;
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	int esc = -1, fit = -1;
	for (int i = 0; i < spp->nbuttons; i++) {
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_ESC")) esc = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_FIT")) fit = i;
	}

	/* base only */
	tdx_config_compose(&c, base, &ps, -1);
	CHECK(c.sensitivity == 1.5 && c.axis_invert[4] == 1 && c.axis_scale[2] == 0.5);
	const struct tdx_keymap *km = tdx_keymap_for(&c, spp, esc);
	CHECK(km && km->keys[0] == KEY_ESC && !tdx_keymap_for(&c, spp, fit));
	CHECK(!strcmp(c.lcd_label[0], "Base"));

	/* base + zeta: overrides, "none" unbinds, invert undone, daemon-wide key ignored */
	tdx_config_compose(&c, base, &ps, 1);
	CHECK(c.sensitivity == 2.0 && c.axis_invert[4] == 0 && c.axis_scale[2] == 0.5);
	CHECK(!tdx_keymap_for(&c, spp, esc));
	km = tdx_keymap_for(&c, spp, fit);
	CHECK(km && km->keys[0] == KEY_F);
	CHECK(!strcmp(c.spnav_socket, "/run/spnav.sock") && !strcmp(c.lcd_label[0], "Prof"));
	int16_t raw[6] = {0, 0, 100, 0, 0, 0};
	int out[6];
	tdx_process_axes(&c, raw, out);
	CHECK(out[2] == 100); /* 100 * 0.5 * 2 */

	/* base + alpha: an axis scaled to 0 is switched off */
	tdx_config_compose(&c, base, &ps, 0);
	int16_t raw2[6] = {0, 0, 0, 300, 0, 0};
	tdx_process_axes(&c, raw2, out);
	CHECK(out[3] == 0 && c.sensitivity == 1.5);

	/* uinput capabilities cover every profile's keys */
	int codes[64];
	int n = tdx_profiles_key_union(base, &ps, codes, 64);
	int has_esc = 0, has_f = 0;
	for (int i = 0; i < n; i++) {
		has_esc |= codes[i] == KEY_ESC;
		has_f |= codes[i] == KEY_F;
	}
	CHECK(n == 2 && has_esc && has_f);

	/* a missing directory is simply "no profiles" */
	CHECK(tdx_profiles_scan("does_not_exist.d", &ps) == 0 && ps.n == 0);

	remove("test_profiles.d/zeta.conf");
	remove("test_profiles.d/alpha.conf");
	remove("test_profiles.d/notes.txt");
	remove("test_profiles.d/.hidden.conf");
	rmdir(dir);
	remove(base);

	char app[64] = "";
	CHECK(tdx_app_from_pid(getpid(), app, sizeof app) == 0 && !strcmp(app, "test_core"));
}

/* Profiles imported from 3DxWare (tdx-extract-3dxware-ui): chords one after
 * another, folders after /etc/tdxd.d, and a user's folder limited to keys. */
static void test_imported_profiles(void)
{
	static struct tdx_config c;
	tdx_config_defaults(&c);
	write_file("test_seq.conf", "key_V3DK_1 = KEY_LEFTALT + KEY_V, KEY_T\n"
				    "key_V3DK_2 = KEY_LEFTSHIFT + KEY_SLASH, KEY_A, KEY_LEFTSHIFT + KEY_R, KEY_ENTER\n"
				    "key_V3DK_3 = KEY_LEFTSHIFT + KEY_LEFTCTRL + KEY_KP7\n");
	CHECK(tdx_config_load(&c, "test_seq.conf") == 0);
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	int k1 = -1, k2 = -1, k3 = -1;
	for (int i = 0; i < spp->nbuttons; i++) {
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_1")) k1 = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_2")) k2 = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_3")) k3 = i;
	}
	const struct tdx_keymap *k = tdx_keymap_for(&c, spp, k1);
	CHECK(k && k->seq && k->keys[0] == KEY_LEFTALT && k->keys[1] == KEY_V && k->keys[2] == KEY_T &&
	      !k->keys[3] && k->step[0] == 0 && k->step[1] == 0 && k->step[2] == 1);
	k = tdx_keymap_for(&c, spp, k2);
	CHECK(k && k->seq && k->keys[5] == KEY_ENTER && k->step[3] == 2 && k->step[4] == 2 && k->step[5] == 3);
	k = tdx_keymap_for(&c, spp, k3); /* a chord of three: held with the button */
	CHECK(k && !k->seq && k->keys[2] == KEY_KP7 && !k->keys[3]);
	char t[64];
	tdx_button_text(&c, spp, k1, t, sizeof t);
	CHECK(strstr(t, ", ") && strstr(t, "+"));  /* "Alt+V, T" */
	remove("test_seq.conf");

	/* folders: /etc/tdxd.d first, a name taken there is skipped later */
	mkdir("test_etc.d", 0755);
	mkdir("test_imp.d", 0755);
	mkdir("test_user.d", 0755);
	write_file("test_etc.d/blender.conf", "match = blender\nkey_V3DK_FIT = KEY_HOME\n");
	write_file("test_imp.d/blender.conf", "match = blender\n");
	write_file("test_imp.d/3dx-blender.conf", "match = blender\nkey_V3DK_FIT = KEY_KPDOT\n");
	write_file("test_imp.d/3dx-prusa-slicer.conf", "match = prusa-slicer\nkey_V3DK_1 = KEY_M\n");
	write_file("test_user.d/3dx-orca.conf",
		   "match = orca-slicer\nsensitivity = 9\ndeadzone = 300\nled = off\n"
		   "key_V3DK_1 = KEY_LEFTCTRL + KEY_A\naction_V3DK_2 = speed_up\nlcd_label_V3DK_1 = Arrange\n");
	CHECK(symlink("../test_imp.d/3dx-prusa-slicer.conf", "test_user.d/3dx-link.conf") == 0);
	static struct tdx_profiles ps;
	CHECK(tdx_profiles_scan("test_etc.d", &ps) == 0 && ps.n == 1);
	CHECK(tdx_profiles_add("test_imp.d", &ps, 0) == 0 && ps.n == 3); /* blender.conf skipped */
	CHECK(tdx_profiles_add("test_user.d", &ps, 1) == 0 && ps.n == 4); /* the symlink too */
	CHECK(!strcmp(ps.p[0].name, "blender") && !strcmp(ps.p[1].name, "3dx-blender"));
	CHECK(tdx_profile_match(&ps, "blender") == 0);       /* yours wins */
	CHECK(tdx_profile_match(&ps, "prusa-slicer") == 2);
	int orca = tdx_profile_match(&ps, "orca-slicer");
	CHECK(orca == 3 && ps.p[3].user);
	tdx_config_compose(&c, "does_not_exist.conf", &ps, orca);
	k = tdx_keymap_for(&c, spp, k1);
	CHECK(k && k->keys[0] == KEY_LEFTCTRL && k->keys[1] == KEY_A);
	CHECK(c.sensitivity == 1.0 && c.deadzone != 300);    /* a user's file: keys only */
	CHECK(tdx_action_for(&c, spp, k2) == TDX_ACT_SPEED_UP);
	tdx_button_text(&c, spp, k1, t, sizeof t);
	CHECK(!strcmp(t, "Arrange"));
	CHECK(tdx_config_load_user_profile(&c, "test_user.d/3dx-link.conf") < 0); /* no symlinks */

	/* key names: the base config names keys 1-10 by number; a profile's name for
	 * a key wins, and a key it rebinds without a name loses the base's name */
	write_file("test_base_lab.conf", "key_V3DK_3 = KEY_LEFTCTRL + KEY_S\nlcd_label_3 = Save\n"
					 "key_V3DK_4 = KEY_LEFTCTRL + KEY_C\nlcd_label_4 = Copy\n"
					 "key_V3DK_5 = KEY_LEFTCTRL + KEY_X\nlcd_label_5 = Cut\n"
					 "lcd_label_V3DK_TOP = Base Top\n");
	write_file("test_imp.d/3dx-lab.conf", "match = lab\n"
					      "key_V3DK_3 = KEY_LEFTCTRL + KEY_LEFTSHIFT + KEY_R\n"
					      "lcd_label_V3DK_3 = Refresh\n"
					      "key_V3DK_4 = KEY_LEFTCTRL + KEY_Y\n"
					      "key_V3DK_TOP = KEY_LEFTSHIFT + KEY_KP7\n");
	write_file("test_imp.d/3dx-lab2.conf", "match = lab2\nlcd_label_3 = Mine\n"
					       "key_V3DK_3 = KEY_F5\n");
	CHECK(tdx_profiles_scan("test_imp.d", &ps) == 0);
	tdx_config_compose(&c, "test_base_lab.conf", &ps, tdx_profile_match(&ps, "lab"));
	int k4 = -1, k5 = -1, top = -1;
	for (int i = 0; i < spp->nbuttons; i++) {
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_4")) k4 = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_5")) k5 = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_TOP")) top = i;
	}
	tdx_button_text(&c, spp, k3, t, sizeof t);
	CHECK(!strcmp(t, "Refresh"));               /* the profile's name, not "Save" */
	tdx_button_text(&c, spp, k4, t, sizeof t);
	CHECK(!strcmp(t, "Redo"));                  /* rebound, no name: not "Copy" */
	tdx_button_text(&c, spp, k5, t, sizeof t);
	CHECK(!strcmp(t, "Cut"));                   /* untouched: the base's name */
	tdx_button_text(&c, spp, top, t, sizeof t);
	CHECK(strcmp(t, "Base Top") != 0);          /* named keys too */
	tdx_config_compose(&c, "test_base_lab.conf", &ps, tdx_profile_match(&ps, "lab2"));
	tdx_button_text(&c, spp, k3, t, sizeof t);
	CHECK(!strcmp(t, "Mine"));                  /* named before the key line: kept */
	remove("test_base_lab.conf");
	remove("test_imp.d/3dx-lab.conf");
	remove("test_imp.d/3dx-lab2.conf");
	remove("test_etc.d/blender.conf");
	remove("test_imp.d/blender.conf");
	remove("test_imp.d/3dx-blender.conf");
	remove("test_imp.d/3dx-prusa-slicer.conf");
	remove("test_user.d/3dx-orca.conf");
	remove("test_user.d/3dx-link.conf");
	rmdir("test_etc.d");
	rmdir("test_imp.d");
	rmdir("test_user.d");
}

static void echo_handler(char *cmd, char *reply, size_t len)
{
	snprintf(reply, len, "got <%s>\n", cmd);
}

static void test_control(void)
{
	const char *path = "/tmp/tdx_test_ctl.sock"; /* not on /mnt: needs a Linux filesystem */
	int lfd = tdx_ctl_listen(path);
	CHECK(lfd >= 0);
	if (lfd < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) { /* server: answer one request */
		struct timespec ts = {0, 50 * 1000 * 1000};
		for (int i = 0; i < 40; i++) { /* the listener is non-blocking */
			nanosleep(&ts, NULL);
			tdx_ctl_serve(lfd, echo_handler);
		}
		_exit(0);
	}
	char reply[256] = "";
	int rc = tdx_ctl_request(path, "focus blender 1234", reply, sizeof reply);
	CHECK(rc == 0 && !strcmp(reply, "got <focus blender 1234>\n"));
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	close(lfd);
	unlink(path);
	CHECK(tdx_ctl_request("no_such.sock", "status", reply, sizeof reply) < 0);
}

static void test_actions(void)
{
	const char *f = "test_actions.conf";
	write_file(f, "action_V3DK_PLUS = speed_up\naction_V3DK_MINUS = Speed_Down\n"
		      "action_V3DK_DOMINANT = dominant\naction_V3DK_ROTATE = rotation\n"
		      "action_V3DK_PANZOOM = panzoom\naction_V3DK_MENU = calibrate\n"
		      "action_V3DK_MENU = none\naction_BUTTON_3 = lcd_next\n"
		      "action_V3DK_FIT = bogus\n");
	static struct tdx_config c;
	tdx_config_defaults(&c);
	CHECK(tdx_config_load(&c, f) == 0);
	unlink(f);
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	int plus = -1, minus = -1, dom = -1, menu = -1, fit = -1;
	for (int i = 0; i < spp->nbuttons; i++) {
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_PLUS")) plus = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_MINUS")) minus = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_DOMINANT")) dom = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_MENU")) menu = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_FIT")) fit = i;
	}
	/* SpacePilot PRO hardware (raw report 0x03, firmware 4.11): the DOM key
	 * sends 0x10000000 and PAN ZOOM 0x08000000, as Base.xml says */
	for (int i = 0; i < spp->nbuttons; i++) {
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_DOMINANT"))
			CHECK(spp->buttons[i].value == 0x10000000u);
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_PANZOOM"))
			CHECK(spp->buttons[i].value == 0x08000000u);
	}
	CHECK(tdx_action_for(&c, spp, plus) == TDX_ACT_SPEED_UP);
	CHECK(tdx_action_for(&c, spp, minus) == TDX_ACT_SPEED_DOWN);
	CHECK(tdx_action_for(&c, spp, menu) == TDX_ACT_NONE); /* "none" replaced calibrate */
	CHECK(tdx_action_for(&c, spp, fit) == TDX_ACT_NONE);  /* unknown name ignored */
	CHECK(tdx_action_for(&c, spp, 2) == TDX_ACT_LCD_NEXT); /* generic BUTTON_3 */
	CHECK(!strcmp(tdx_action_name(TDX_ACT_PANZOOM), "panzoom"));
	CHECK(tdx_action_from_name("ROTATION") == TDX_ACT_ROTATION && tdx_action_from_name("x") < 0);

	c.sensitivity = 1.0;
	CHECK(tdx_apply_action(&c, TDX_ACT_SPEED_UP) && c.sensitivity == 1.25);
	CHECK(tdx_apply_action(&c, TDX_ACT_SPEED_DOWN) && c.sensitivity == 1.0);
	for (int i = 0; i < 40; i++)
		tdx_apply_action(&c, TDX_ACT_SPEED_DOWN);
	CHECK(c.sensitivity == 0.05);
	CHECK(tdx_apply_action(&c, TDX_ACT_DOMINANT) && c.dominant == 1);
	CHECK(tdx_apply_action(&c, TDX_ACT_DOMINANT) && c.dominant == 0);
	CHECK(tdx_apply_action(&c, TDX_ACT_ROTATION) && c.disable_rotation == 1);
	CHECK(tdx_apply_action(&c, TDX_ACT_PANZOOM) && c.disable_translation == 1);
	CHECK(!tdx_apply_action(&c, TDX_ACT_CALIBRATE) && !tdx_apply_action(&c, TDX_ACT_LCD_NEXT));
	(void)dom;
}

static void test_lcdmgr(void)
{
	/* the database LCDMon 3.06 wrote on the SpacePilot PRO test machine */
	const char *f = "test_lgLcddb.txt";
	write_file(f,
		   "[General]\r\nWWidth=550\r\nShowTrayIcon=1\r\nVersion=30\r\nRestoreSize=0\r\n\r\n"
		   "[DEVICE_0]\r\nBrightness=89\r\nFamilyId=32\r\nAlerts=1\r\nInvertColors=0\r\n"
		   "DeviceId=110064\r\nManualModeApplet=\"C:\\\\Program Files\\\\Logitech\\\\GamePanel "
		   "Software\\\\LCD Manager\\\\Applets\\\\3DxLCD10SPPRO.exe\",\"Function Keys\"\r\n"
		   "QuickSwitch=0\r\nFamilyName=3Dconnexion SpacePilot PRO\r\nAlgorithm=1\r\nDelay=8\r\n"
		   "Contrast=0\r\n\r\n[Application_0]\r\n0=F32,I110064,E1\r\n\r\n[Application_1]\r\n"
		   "0=F32,I110064,E0\r\n\r\n[ClientApplications]\r\n"
		   "0=\"C:\\\\Applets\\\\3DxLCD10SPPRO.exe\",\"Function Keys\",E1,A1,S1,L1,C2\r\n"
		   "1=\"C:\\\\Applets\\\\zz3DxLCD50RSSReader.exe\",\"RSS Feeds\",E0,A1,S1,L0,C2\r\n");
	static struct lm_db db, db2;
	lm_db_defaults(&db);
	CHECK(lm_db_load(&db, f) == 0);
	CHECK(db.brightness == 89 && db.family_id == 32 && db.device_id == 110064 && db.delay == 8);
	CHECK(db.algorithm == 1 && db.alerts == 1 && db.quickswitch == 0 && db.wwidth == 550);
	CHECK(!strcmp(db.manual_name, "Function Keys"));
	CHECK(strstr(db.manual_path, "LCD Manager\\Applets\\3DxLCD10SPPRO.exe") != NULL);
	CHECK(db.nprog == 2 && !strcmp(db.prog[1].name, "RSS Feeds"));
	CHECK(!strcmp(db.prog[0].path, "C:\\Applets\\3DxLCD10SPPRO.exe"));
	CHECK(db.prog[0].enabled && db.prog[0].autostart && db.prog[0].persistent &&
	      db.prog[0].allow_fg && db.prog[0].caps == 2 && db.prog[0].dev_enabled);
	CHECK(!db.prog[1].enabled && !db.prog[1].allow_fg && !db.prog[1].dev_enabled);
	CHECK(lm_db_save(&db, f) == 0);
	lm_db_defaults(&db2);
	CHECK(lm_db_load(&db2, f) == 0);
	CHECK(db2.nprog == 2 && db2.brightness == 89 && !strcmp(db2.manual_name, "Function Keys") &&
	      !strcmp(db2.prog[0].path, db.prog[0].path) && !db2.prog[1].dev_enabled);
	unlink(f);
	CHECK(lm_db_add(&db, "/usr/bin/x", "X", 2, 0, 0) == 2 && !db.prog[2].persistent);
	CHECK(lm_db_add(&db, "/usr/bin/x", "X", 2, 1, 0) == 2 && db.prog[2].persistent);

	/* soft-button menu (LCDMon FUN_14007fadc / FUN_140084a10) */
	struct lm_menu m = {0};
	int commit = -1;
	CHECK(lm_menu_buttons(&m, TDX_SB_UP, 3, 0, 0, 0, &commit) == LM_MENU_FORWARD);
	CHECK(lm_menu_buttons(&m, 0, 3, 0, 0, 0, &commit) == LM_MENU_FORWARD);
	CHECK(lm_menu_buttons(&m, LM_KEY_LCD, 3, 1, 0, 0, &commit) == LM_MENU_REDRAW && m.open &&
	      m.sel == 1);
	CHECK(lm_menu_buttons(&m, 0, 3, 1, 0, 10, &commit) == LM_MENU_NONE);
	CHECK(lm_menu_buttons(&m, TDX_SB_DOWN, 3, 1, 0, 20, &commit) == LM_MENU_REDRAW && m.sel == 2);
	lm_menu_buttons(&m, 0, 3, 1, 0, 30, &commit);
	CHECK(lm_menu_buttons(&m, TDX_SB_DOWN, 3, 1, 0, 40, &commit) == LM_MENU_REDRAW && m.sel == 0);
	lm_menu_buttons(&m, 0, 3, 1, 0, 50, &commit);
	CHECK(lm_menu_buttons(&m, TDX_SB_UP, 3, 1, 0, 60, &commit) == LM_MENU_REDRAW && m.sel == 2);
	lm_menu_buttons(&m, 0, 3, 1, 0, 70, &commit);
	CHECK(lm_menu_buttons(&m, TDX_SB_OK, 3, 1, 0, 80, &commit) == LM_MENU_COMMIT && commit == 2 &&
	      !m.open);
	lm_menu_buttons(&m, 0, 3, 1, 0, 90, &commit);
	CHECK(lm_menu_buttons(&m, LM_KEY_LCD, 3, 1, 0, 100, &commit) == LM_MENU_REDRAW);
	lm_menu_buttons(&m, 0, 3, 1, 0, 110, &commit);
	CHECK(lm_menu_tick(&m, 2000) == 0 && lm_menu_tick(&m, 2200) == 1 && !m.open);
	CHECK(lm_menu_buttons(&m, LM_KEY_LCD, 3, 2, 1, 3000, &commit) == LM_MENU_COMMIT &&
	      commit == 0); /* quick switch wraps to the first program */

	/* display selection */
	struct lm_sched s = {.shown = -1};
	struct lm_cand c[3] = {{.prog = 0, .has_frame = 1, .priority = 128},
			       {.prog = 1, .has_frame = 1, .priority = 128},
			       {.prog = 2, .has_frame = 1, .priority = 0}};
	CHECK(lm_pick(&s, c, 3, 1, 8, 1, -1, 0) == 0);  /* manual, nothing chosen */
	CHECK(lm_pick(&s, c, 3, 1, 8, 1, 1, 0) == 1);   /* manual choice */
	c[0].priority = 255, c[0].alert_time = 5;
	CHECK(lm_pick(&s, c, 3, 1, 8, 1, 1, 0) == 0);   /* alert pre-empts */
	CHECK(lm_pick(&s, c, 3, 1, 8, 0, 1, 0) == 1);   /* ...unless Alerts=0 */
	c[0].priority = 128;
	c[1].foreground = 1, c[1].fg_time = 9;
	CHECK(lm_pick(&s, c, 3, 1, 8, 1, 0, 0) == 1);   /* foreground request */
	c[1].foreground = 0;
	struct lm_sched r = {.shown = -1};
	CHECK(lm_pick(&r, c, 3, 2, 2, 1, -1, 1000) == 0);
	CHECK(lm_pick(&r, c, 3, 2, 2, 1, -1, 2500) == 0);
	CHECK(lm_pick(&r, c, 3, 2, 2, 1, -1, 3100) == 1); /* 2 s later: next */
	CHECK(lm_pick(&r, c, 3, 2, 2, 1, -1, 5200) == 0); /* skips the idle (priority 0) one */
	CHECK(lm_pick(&r, c, 0, 2, 2, 1, -1, 6000) == -1);
	CHECK(tdx_lcd_softbuttons(0x80) == TDX_SB_UP && tdx_lcd_softbuttons(0x08) == TDX_SB_OK &&
	      tdx_lcd_softbuttons(0x01) == TDX_SB_SPECIAL && tdx_lcd_softbuttons(0x46) ==
	      (TDX_SB_DOWN | TDX_SB_MENU | TDX_SB_CANCEL));
}

static void test_lcdcfg(void)
{
	const char *f = "test_lcdcfg.conf";
	write_file(f, "key_V3DK_1 = KEY_LEFTCTRL+KEY_C\nkey_V3DK_2 = KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_P\n"
		      "key_V3DK_ESC = KEY_ESC\naction_V3DK_PLUS = speed_up\nlcd_label_3 = Render\n"
		      "disable_translation = yes\n");
	static struct tdx_config c;
	tdx_config_defaults(&c);
	CHECK(tdx_config_load(&c, f) == 0);
	unlink(f);
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	char t[64];
	int idx[6] = {-1, -1, -1, -1, -1, -1};
	const char *ids[6] = {"V3DK_1", "V3DK_2", "V3DK_3", "V3DK_ESC", "V3DK_PLUS", "V3DK_TOP"};
	for (int i = 0; i < spp->nbuttons; i++)
		for (int k = 0; k < 6; k++)
			if (!strcmp(spp->buttons[i].v3dk, ids[k]))
				idx[k] = i;
	tdx_button_text(&c, spp, idx[0], t, sizeof t);
	CHECK(!strcmp(t, "Copy"));
	tdx_button_text(&c, spp, idx[1], t, sizeof t);
	CHECK(!strcmp(t, "Ctrl+Shift+P"));
	tdx_button_text(&c, spp, idx[2], t, sizeof t);
	CHECK(!strcmp(t, "Render"));        /* explicit lcd_label_3 */
	tdx_button_text(&c, spp, idx[3], t, sizeof t);
	CHECK(!strcmp(t, "Esc Key"));
	tdx_button_text(&c, spp, idx[4], t, sizeof t);
	CHECK(!strcmp(t, "Increase Speed"));
	tdx_button_text(&c, spp, idx[5], t, sizeof t);
	CHECK(!strcmp(t, "Top View"));      /* sent to the application */
	static char xml[16384];
	struct tdx_lcdcfg_info info = {.app = "blender & co", .pressed = "Copy", .slider = 125,
				       .lcd_level = 2, .led = 100};
	int n = tdx_lcdcfg_xml(xml, sizeof xml, &c, spp, 0x46d, 0xc629, &info);
	CHECK(n > 0 && strstr(xml, "<ProductID>c629</ProductID>"));
	CHECK(strstr(xml, "<PanZoom>false</PanZoom>") && strstr(xml, "<Rotation>true</Rotation>"));
	CHECK(strstr(xml, "<ApplicationName>blender &amp; co</ApplicationName>") != NULL);
	CHECK(strstr(xml, "<ButtonPressed>Copy</ButtonPressed>") && strstr(xml, "<Slider>125</Slider>"));
	CHECK(strstr(xml, "<V3DK>V3DK_1</V3DK>\n\t\t<Text>Copy</Text>") != NULL);
	CHECK(tdx_lcdcfg_xml(xml, 64, &c, spp, 0x46d, 0xc629, &info) == -1);
}

/* The shipped FreeCAD profile: view keys -> FreeCAD's shortcuts, a tapped
 * sequence for Fit All, and the LCD keeps the view names. */
static void test_freecad_profile(void)
{
	static struct tdx_config c;
	tdx_config_defaults(&c);
	CHECK(tdx_config_load(&c, "packaging/profiles/freecad.conf") == 0);
	const struct tdx_device_def *spp = tdx_device_lookup(0x046D, 0xC629);
	int top = -1, fit = -1, roll = -1, iso = -1;
	for (int i = 0; i < spp->nbuttons; i++) {
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_TOP")) top = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_FIT")) fit = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_ROLL_CW")) roll = i;
		if (!strcmp(spp->buttons[i].v3dk, "V3DK_ISO1")) iso = i;
	}
	const struct tdx_keymap *k = tdx_keymap_for(&c, spp, top);
	CHECK(k && k->keys[0] == KEY_2 && !k->keys[1] && !k->seq);
	k = tdx_keymap_for(&c, spp, iso);
	CHECK(k && k->keys[0] == KEY_0);
	k = tdx_keymap_for(&c, spp, fit);   /* "KEY_V, KEY_F": tapped one after the other */
	CHECK(k && k->seq && k->keys[0] == KEY_V && k->keys[1] == KEY_F && !k->keys[2]);
	k = tdx_keymap_for(&c, spp, roll);  /* "KEY_LEFTSHIFT + KEY_RIGHT": a held chord */
	CHECK(k && !k->seq && k->keys[0] == KEY_LEFTSHIFT && k->keys[1] == KEY_RIGHT);
	char t[64];
	tdx_button_text(&c, spp, top, t, sizeof t);
	CHECK(!strcmp(t, "Top View"));      /* lcd_label_V3DK_TOP, not "2" */
	tdx_button_text(&c, spp, fit, t, sizeof t);
	CHECK(!strcmp(t, "Fit All"));
	CHECK(c.nnamed_label == 10);
	/* the focused application's own name for a key comes first (app-buttons) */
	snprintf(c.app_label[0].v3dk, sizeof c.app_label[0].v3dk, "V3DK_TOP");
	snprintf(c.app_label[0].text, sizeof c.app_label[0].text, "Create sketch");
	c.napp_label = 1;
	tdx_button_text(&c, spp, top, t, sizeof t);
	CHECK(!strcmp(t, "Create sketch"));
	c.napp_label = 0;
	/* without a label a sequence reads "V, F" */
	c.nnamed_label = 0;
	tdx_button_text(&c, spp, fit, t, sizeof t);
	CHECK(!strcmp(t, "V, F"));
}

/* Focus: a script is named after the script, not its interpreter (tdx-settings
 * runs as python3.14), and tdxd's own programs are recognised. */
static void test_focus_names(void)
{
	char dir[] = "/tmp/tdx-focus-XXXXXX";
	if (!mkdtemp(dir))
		return;
	char path[64], name[64];
	snprintf(path, sizeof path, "%s/tdx-settings", dir);
	write_file(path, "sleep 5\n");
	pid_t pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", path, (char *)NULL);
		_exit(127);
	}
	usleep(200000);
	CHECK(tdx_app_from_pid(pid, name, sizeof name) == 0 && !strcmp(name, "tdx-settings"));
	CHECK(tdx_is_own_app(name));
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", "-c", "sleep 5", (char *)NULL);
		_exit(127);
	}
	usleep(200000);
	CHECK(tdx_app_from_pid(pid, name, sizeof name) == 0 && strcmp(name, "tdx-settings")); /* -c: no script */
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(path);
	rmdir(dir);
	CHECK(!tdx_is_own_app("freecad") && !tdx_is_own_app("blender") && tdx_is_own_app("tdx-lcdmanager"));
}

/* Captured from a Universal Receiver (256f:c652) with a CadMouse Compact
 * Wireless paired, 2026-11-22. */
static void test_receiver(void)
{
	const uint8_t self[8] = {0x42, 0x59, 0x00, 0x00, 0x48, 0xf7, 0xba, 0x8b};
	const uint8_t empty[8] = {0x43, 0, 0, 0, 0, 0, 0, 0};
	struct tdx_rcv_slot s;
	CHECK(tdx_rcv_parse_slot(self, sizeof self, &s) == 1);
	CHECK(s.code == 'Y' && s.pid == 0xC658 && s.cls == TDX_RCV_2DMOUSE);
	CHECK(!strcmp(s.id, "0048f7ba8b"));
	CHECK(!strcmp(s.serial, "Y1224194699")); /* as 3DxWare's log and Advanced Settings show it */
	CHECK(tdx_rcv_parse_slot(empty, sizeof empty, &s) == 0 && s.code == 0);
	CHECK(tdx_rcv_parse_slot(self, 4, &s) == -1);
	const uint8_t wrong[8] = {0x50, 0x59};
	CHECK(tdx_rcv_parse_slot(wrong, sizeof wrong, &s) == -1);
	int cls;
	CHECK(tdx_rcv_code_pid(0x5255, &cls) == 0xC652 && cls == TDX_RCV_RECEIVER); /* "UR" */
	CHECK(tdx_rcv_code_pid('A', &cls) == 0xC62E && cls == TDX_RCV_3DMOUSE);
	CHECK(tdx_rcv_code_pid(0x1234, &cls) == 0 && cls == 0);
	/* the CadMouse configuration: 3DxWare's default block, byte for byte, and its
	 * DPI / polling rate / Smart Scrolling variants (from 3DxWare's log) */
	uint8_t rep[TDX_RCV_MOUSE_CONFIG_LEN];
	const uint8_t def[TDX_RCV_MOUSE_CONFIG_LEN] = {
		0x10, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x0a, 0x0b, 0x0c, 0x0c, 0x0e, 0x0d, 0x2f, 0x00, 0x1e, 0x00, 0x00,
		0x00, 0x01};
	struct tdx_config mc;
	tdx_config_defaults(&mc);
	const struct tdx_device_def *cmw = tdx_device_lookup(0x256F, 0xC658);
	uint8_t map[7];
	CHECK(tdx_rcv_mouse_map(&mc, cmw, map) == 0x40); /* only the radial menu reports */
	tdx_rcv_mouse_config(rep, 1400, 1000, 1, map);
	CHECK(!memcmp(rep, def, sizeof def));
	mc.mouse_swap_buttons = 1; /* 3DxWare's Swap Middle / Right: 10 12 11 ... */
	tdx_rcv_mouse_map(&mc, cmw, map);
	CHECK(map[0] == 10 && map[1] == 12 && map[2] == 11 && map[3] == 12);
	mc.mouse_swap_buttons = 0;
	{
		char p[] = "/tmp/tdx-test-cm.conf";
		FILE *f = fopen(p, "w");
		fputs("key_V2DK_FORWARD = KEY_LEFTCTRL + KEY_Z\nkey_V2DK_RADIALMENU = BTN_MIDDLE\n"
		      "key_V2DK_WHEEL = BTN_SIDE\nkey_BUTTON_6 = KEY_A\nkey_V2DK_LEFT = KEY_B\n", f);
		fclose(f);
		CHECK(tdx_config_load(&mc, p) == 0);
		unlink(p);
	}
	unsigned vend = tdx_rcv_mouse_map(&mc, cmw, map);
	/* forward -> keys (reports, 45), radial -> middle (12), wheel -> back (13),
	 * button 6 keeps back: BUTTON_6 is not a CadMouse binding; left stays left */
	CHECK(vend == 0x10 && map[4] == 45 && map[6] == 12 && map[3] == 13 && map[5] == 13 &&
	      map[0] == 10);
	CHECK(tdx_rcv_mouse_binding(&mc, cmw, 4) && tdx_rcv_mouse_binding(&mc, cmw, 4)->keys[1]);
	CHECK(tdx_rcv_mouse_binding(&mc, cmw, 5) == NULL);
	{
		char p[] = "/tmp/tdx-test-cm2.conf";
		FILE *f = fopen(p, "w");
		fputs("action_V2DK_BACKWARD = speed_up\naction_V2DK_WHEEL = disabled\n"
		      "key_V2DK_RADIALMENU = KEY_MICMUTE\n", f);
		fclose(f);
		CHECK(tdx_config_load(&mc, p) == 0);
		unlink(p);
	}
	/* a driver action or Disabled: the button reports (40 + n), tdxd acts */
	vend = tdx_rcv_mouse_map(&mc, cmw, map);
	CHECK(map[5] == 46 && map[3] == 44 && map[6] == 47 && (vend & 0x68) == 0x68);
	CHECK(tdx_rcv_mouse_action(&mc, cmw, 5) == TDX_ACT_SPEED_UP &&
	      tdx_rcv_mouse_action(&mc, cmw, 3) == TDX_ACT_DISABLED &&
	      tdx_rcv_mouse_action(&mc, cmw, 4) == TDX_ACT_NONE);
	CHECK(tdx_rcv_mouse_binding(&mc, cmw, 6)->keys[0] == KEY_MICMUTE);
	{
		char p[] = "/tmp/tdx-test-cm3.conf";
		FILE *f = fopen(p, "w");
		fputs("action_V2DK_FORWARD = commontools\n", f);
		fclose(f);
		CHECK(tdx_config_load(&mc, p) == 0);
		unlink(p);
	}
	/* Common Tools (the radial menu) on another button: it reports, tdxd shows it */
	vend = tdx_rcv_mouse_map(&mc, cmw, map);
	CHECK(tdx_action_from_name("commontools") == TDX_ACT_COMMONTOOLS &&
	      !strcmp(tdx_action_name(TDX_ACT_COMMONTOOLS), "commontools") &&
	      tdx_rcv_mouse_action(&mc, cmw, 4) == TDX_ACT_COMMONTOOLS && map[4] == 45 &&
	      (vend & 0x10));
	CHECK(tdx_key_from_name("KEY_SHIFT") == KEY_LEFTSHIFT && tdx_key_from_name("ctrl") == KEY_LEFTCTRL &&
	      tdx_key_from_name("KEY_ALT") == KEY_LEFTALT && tdx_key_from_name("KEY_SHIFTX") < 0);
	CHECK(tdx_action_from_name("numpad") == TDX_ACT_NUMPAD &&
	      !strcmp(tdx_action_name(TDX_ACT_NUMPAD), "numpad"));
	{
		char p[] = "/tmp/tdx-test-cm4.conf";
		FILE *f = fopen(p, "w");
		fputs("action_V2DK_BACKWARD = radial:r1\nradial_r1_name = My tools\n"
		      "radial_r1_layout = 8\nradial_r1_1 = keys:LEFTCTRL+Z\nradial_r1_name = Mine\n"
		      "action_V2DK_WHEEL = radial:bad-id\n", f);
		fclose(f);
		CHECK(tdx_config_load(&mc, p) == 0);
		unlink(p);
	}
	/* a user radial menu: the button's action keeps the menu ID; its lines stay
	 * as written (a later one overrides) for tdx-radial */
	int rm = -1;
	for (int i = 0; i < mc.nactions; i++)
		if (!strcmp(mc.actions[i].v3dk, "V2DK_BACKWARD"))
			rm = i;
	int nm = 0, named = 0;
	for (int i = 0; i < mc.nradial; i++) {
		nm += !strncmp(mc.radial[i].key, "radial_r1_", 10);
		named |= !strcmp(mc.radial[i].key, "radial_r1_name") && !strcmp(mc.radial[i].val, "Mine");
	}
	CHECK(rm >= 0 && mc.actions[rm].action == TDX_ACT_RADIAL && !strcmp(mc.actions[rm].menu, "r1") &&
	      tdx_rcv_mouse_action(&mc, cmw, 5) == TDX_ACT_RADIAL && nm == 3 && named &&
	      tdx_action_from_name("radial:bad-id") < 0 && tdx_action_from_name("radial") < 0);
	tdx_config_defaults(&mc);
	tdx_rcv_mouse_map(&mc, cmw, map);
	tdx_rcv_mouse_config(rep, 7200, 125, 0, map);
	CHECK(rep[2] == 144 && rep[31] == 8 && rep[4] == 0x01 && rep[5] == 0xff && rep[7] == 0);
	tdx_rcv_mouse_config(rep, 200, 500, 1, map);
	CHECK(rep[2] == 4 && rep[31] == 2);
	tdx_rcv_mouse_config(rep, 1000, 250, 1, map);
	CHECK(rep[2] == 20 && rep[31] == 4);
	CHECK(mc.mouse_dpi == 1400 && mc.mouse_polling_rate == 1000 && mc.mouse_smart_scrolling);
	const struct tdx_device_def *cm = tdx_device_lookup(0x256F, 0xC658);
	CHECK(cm && !strcmp(cm->name, "CadMouse Compact Wireless"));

	/* pairing commands, as sent to the receiver (2026-11-22) */
	uint8_t pc[5];
	tdx_rcv_cmd_pairing(pc, TDX_RCV_PAIR_EUCLID);
	CHECK(!memcmp(pc, "\x41\x02\x02\x00\x00", 5));
	tdx_rcv_cmd_pairing(pc, TDX_RCV_PAIR_OFF);
	CHECK(!memcmp(pc, "\x41\x02\x00\x00\x00", 5));
	tdx_rcv_cmd_unpair(pc, 4);
	CHECK(!memcmp(pc, "\x41\x04\x04\x00\x00", 5));

	int lvl, chg;
	const uint8_t bat[3] = {0x17, 0x34, 0x00};
	CHECK(tdx_rcv_parse_battery(bat, sizeof bat, &lvl, &chg) == 1 && lvl == 52 && chg == 0);
	const uint8_t charging[3] = {0x17, 0xc8, 0x01};
	CHECK(tdx_rcv_parse_battery(charging, sizeof charging, &lvl, &chg) == 1 && lvl == 100 && chg);
	const uint8_t unknown[3] = {0x17, 0x00, 0x00};
	CHECK(tdx_rcv_parse_battery(unknown, sizeof unknown, &lvl, &chg) == 1 && lvl == -1);
	const uint8_t motion[9] = {0x1b, 0x00, 0x34, 0x00, 0x6d, 0xff, 0, 0, 0};
	CHECK(tdx_rcv_parse_battery(motion, sizeof motion, &lvl, &chg) == 0);
}

int main(int argc, char **argv)
{
	vec_dir = argc > 1 ? argv[1] : NULL;
	test_receiver();
	test_rezero();
	test_actions();
	test_lcdmgr();
	test_lcdcfg();
	test_freecad_profile();
	test_focus_names();
	test_profiles();
	test_imported_profiles();
	test_control();
	test_enterprise_lcd();
	test_giotto_lcd();
	test_giotto_slide();
	test_giotto_slide_model();
	test_giotto_move();
	test_giotto_rect();
	test_giotto_bmp();
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
