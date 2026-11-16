#include "process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void tdx_process_axes(const struct tdx_config *cfg, const int16_t raw[6], int out[6])
{
	for (int i = 0; i < 6; i++) {
		int v = raw[cfg->axis_map[i]];
		if (abs(v) <= cfg->deadzone)
			v = 0;
		out[i] = (int)((double)v * cfg->axis_scale[i] * cfg->sensitivity);
	}
	if (cfg->disable_translation)
		out[0] = out[1] = out[2] = 0;
	if (cfg->disable_rotation)
		out[3] = out[4] = out[5] = 0;
	if (cfg->dominant) {
		int best = 0;
		for (int i = 1; i < 6; i++)
			if (abs(out[i]) > abs(out[best]))
				best = i;
		for (int i = 0; i < 6; i++)
			if (i != best)
				out[i] = 0;
	}
}

const struct tdx_keymap *tdx_keymap_for(const struct tdx_config *cfg,
					const struct tdx_device_def *dev, int idx)
{
	char generic[32];
	snprintf(generic, sizeof generic, "BUTTON_%d", idx + 1);
	const char *v3dk = (dev && idx < dev->nbuttons) ? dev->buttons[idx].v3dk : NULL;
	for (int i = 0; i < cfg->nkeymap; i++) {
		const char *n = cfg->keymap[i].v3dk;
		if ((v3dk && !strcasecmp(n, v3dk)) || !strcasecmp(n, generic))
			return &cfg->keymap[i];
	}
	return NULL;
}

/* LED index for a configured name: V3DK id via the device's <LEDNumber>, or "LED_n". */
static int led_index_for(const struct tdx_device_def *dev, const char *name)
{
	if (!strncasecmp(name, "LED_", 4)) {
		char *end;
		long n = strtol(name + 4, &end, 10);
		return (!*end && n >= 0 && n < TDX_RGB_RING_FIRST + TDX_RGB_NUM_RING) ? (int)n : -1;
	}
	for (int i = 0; dev && i < dev->nbuttons; i++)
		if (dev->buttons[i].led >= 0 && dev->buttons[i].v3dk &&
		    !strcasecmp(dev->buttons[i].v3dk, name))
			return dev->buttons[i].led;
	return -1;
}

int tdx_rgb_plan(const struct tdx_config *cfg, const struct tdx_device_def *dev,
		 uint8_t out[][8], int max)
{
	int n = 0;
	if (!tdx_has_rgb_leds(dev) || max < 1)
		return 0;
	int any_color = cfg->led_button_color >= 0 || cfg->led_ring_color >= 0 || cfg->nledcolor;
	if (cfg->led == 0) {
		tdx_rgb_report(TDX_RGB_ALL_OFF, 0, out[n++]);
		return n;
	}
	if (cfg->led < 0 && !any_color)
		return 0;

	uint32_t base = cfg->led_button_color >= 0 ? (uint32_t)cfg->led_button_color : 0xFFFFFF;
	uint32_t led[TDX_RGB_RING_FIRST + TDX_RGB_NUM_RING];
	int ring_set = cfg->led_ring_color >= 0 || cfg->led == 1;
	uint32_t ring = cfg->led_ring_color >= 0 ? (uint32_t)cfg->led_ring_color : 0xFFFFFF;
	for (int i = 0; i < TDX_RGB_NUM_BUTTONS; i++)
		led[i] = base;
	int ring_individual = 0;
	for (int i = TDX_RGB_NUM_BUTTONS; i < TDX_RGB_RING_FIRST + TDX_RGB_NUM_RING; i++)
		led[i] = ring;
	for (int i = 0; i < cfg->nledcolor; i++) {
		int idx = led_index_for(dev, cfg->ledcolor[i].v3dk);
		if (idx < 0) {
			fprintf(stderr, "tdxd: no LED for '%s' on %s\n", cfg->ledcolor[i].v3dk, dev->name);
			continue;
		}
		led[idx] = cfg->ledcolor[i].rgb;
		if (idx >= TDX_RGB_RING_FIRST)
			ring_individual = 1;
	}

	int uniform = 1;
	for (int i = 1; i < TDX_RGB_NUM_BUTTONS; i++)
		if (led[i] != led[0])
			uniform = 0;
	if (uniform) {
		tdx_rgb_report(TDX_RGB_ALL_BUTTONS, led[0], out[n++]);
	} else {
		for (int i = 0; i < TDX_RGB_NUM_BUTTONS && n < max; i++)
			tdx_rgb_report((uint8_t)i, led[i], out[n++]);
	}
	if (ring_individual) {
		for (int i = TDX_RGB_RING_FIRST; i < TDX_RGB_RING_FIRST + TDX_RGB_NUM_RING && n < max; i++)
			tdx_rgb_report((uint8_t)i, led[i], out[n++]);
	} else if (ring_set && n < max) {
		tdx_rgb_report(TDX_RGB_RING, ring, out[n++]);
	}
	return n;
}

void tdx_render_keys_page(struct tdx_fb *fb, const struct tdx_config *cfg,
			  const char *devname, int page)
{
	const uint32_t bg = 0x101418, bar = 0x1F6FB2, fg = 0xE6E6E6, num = 0xFFC040;
	gfx_fill(fb, 0, 0, fb->w, fb->h, bg);
	gfx_fill(fb, 0, 0, fb->w, 28, bar);
	gfx_text(fb, 8, 7, devname ? devname : "3DCONNEXION", 2, 0xFFFFFF);
	/* 5 keys per page; page 0 = 1..5, page 1 = 6..10 */
	for (int r = 0; r < 5; r++) {
		int k = page * 5 + r;
		int y = 38 + r * 40;
		char n[12];
		snprintf(n, sizeof n, "%d", k + 1);
		gfx_fill(fb, 6, y, 36, 32, 0x2A3038);
		gfx_text(fb, 6 + (36 - gfx_text_width(n, 3)) / 2 + 1, y + 6, n, 3, num);
		gfx_text(fb, 52, y + 6, cfg->lcd_label[k], 3, fg);
	}
}

void tdx_render_ent_page(struct tdx_fb *fb, const struct tdx_config *cfg, const char *devname)
{
	/* 6x2 grid of key labels; scales with the panel (640x150 Enterprise, 1280x416 Giotto) */
	const uint32_t bg = 0x101418, bar = 0x1F6FB2, fg = 0xE6E6E6, num = 0xFFC040, cell = 0x2A3038;
	const int s = fb->h >= 400 ? 4 : 2;          /* font scale */
	const int bar_h = 11 * s, pad = 2 * s;
	gfx_fill(fb, 0, 0, fb->w, fb->h, bg);
	gfx_fill(fb, 0, 0, fb->w, bar_h, bar);
	gfx_text(fb, 3 * s, 2 * s, devname ? devname : "3DCONNEXION", s, 0xFFFFFF);
	const int cols = 6, cw = fb->w / cols, top = bar_h + pad, rh = (fb->h - top) / 2;
	const int maxch = (cw - 8 * s) / (6 * s);
	for (int k = 0; k < 12; k++) {
		int x = (k % cols) * cw, y = top + (k / cols) * rh;
		char n[12], lab[24];
		snprintf(n, sizeof n, "%d", k + 1);
		gfx_fill(fb, x + pad, y + pad / 2, cw - 2 * pad, rh - pad, cell);
		gfx_text(fb, x + 4 * s, y + 3 * s, n, s, num);
		snprintf(lab, sizeof lab, "%.*s", maxch > 0 && maxch < 23 ? maxch : 23, cfg->lcd_label[k]);
		gfx_text(fb, x + 4 * s, y + rh / 2 + s, lab, s, fg);
	}
}
