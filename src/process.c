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
