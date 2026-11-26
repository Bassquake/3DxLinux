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
		double s = cfg->axis_scale[i] * (cfg->axis_invert[i] ? -1.0 : 1.0);
		out[i] = (int)((double)v * s * cfg->sensitivity);
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
			return cfg->keymap[i].keys[0] ? &cfg->keymap[i] : NULL; /* "none" = unmapped */
	}
	return NULL;
}

int tdx_action_for(const struct tdx_config *cfg, const struct tdx_device_def *dev, int idx)
{
	char generic[32];
	snprintf(generic, sizeof generic, "BUTTON_%d", idx + 1);
	const char *v3dk = (dev && idx < dev->nbuttons) ? dev->buttons[idx].v3dk : NULL;
	for (int i = 0; i < cfg->nactions; i++) {
		const char *n = cfg->actions[i].v3dk;
		if ((v3dk && !strcasecmp(n, v3dk)) || !strcasecmp(n, generic))
			return cfg->actions[i].action;
	}
	return TDX_ACT_NONE;
}

/* 3DxWare's speed buttons step the global speed; 1.25x per press, clamped. */
#define TDX_SPEED_STEP 1.25
#define TDX_SPEED_MIN 0.05
#define TDX_SPEED_MAX 20.0

int tdx_apply_action(struct tdx_config *cfg, int action)
{
	switch (action) {
	case TDX_ACT_SPEED_UP:
		cfg->sensitivity *= TDX_SPEED_STEP;
		if (cfg->sensitivity > TDX_SPEED_MAX)
			cfg->sensitivity = TDX_SPEED_MAX;
		return 1;
	case TDX_ACT_SPEED_DOWN:
		cfg->sensitivity /= TDX_SPEED_STEP;
		if (cfg->sensitivity < TDX_SPEED_MIN)
			cfg->sensitivity = TDX_SPEED_MIN;
		return 1;
	case TDX_ACT_DOMINANT:
		cfg->dominant = !cfg->dominant;
		return 1;
	case TDX_ACT_ROTATION:
		cfg->disable_rotation = !cfg->disable_rotation;
		return 1;
	case TDX_ACT_PANZOOM:
		cfg->disable_translation = !cfg->disable_translation;
		return 1;
	default:
		return 0;
	}
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
			fprintf(stderr, "3dxlinuxd: no LED for '%s' on %s\n", cfg->ledcolor[i].v3dk, dev->name);
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

void tdx_render_ent_page(struct tdx_fb *fb, const struct tdx_config *cfg, const char *devname,
			 int page, int npages)
{
	/* 6x2 grid of key labels; scales with the panel (640x150 Enterprise, 1280x416 Giotto).
	 * On the 416-px panel the grid occupies exactly the slide band (rows 60..400),
	 * so a slide moves the whole grid and the title bar stays fixed. */
	const uint32_t bg = 0x101418, bar = 0x1F6FB2, fg = 0xE6E6E6, num = 0xFFC040, cell = 0x2A3038;
	const int big = fb->h >= 400;
	const int s = big ? 4 : 2;                   /* font scale */
	const int bar_h = big ? 60 : 11 * s, pad = 2 * s;
	const int top = big ? 60 : bar_h + pad;
	const int rh = big ? 341 / 2 : (fb->h - top) / 2;
	if (page < 0 || page * 12 >= TDX_MAX_LABELS)
		page = 0;
	gfx_fill(fb, 0, 0, fb->w, fb->h, bg);
	gfx_fill(fb, 0, 0, fb->w, bar_h, bar);
	gfx_text(fb, 3 * s, (bar_h - 7 * s) / 2, devname ? devname : "3DCONNEXION", s, 0xFFFFFF);
	if (npages > 1) { /* page dots, right-aligned in the title bar */
		for (int i = 0; i < npages; i++) {
			int d = 3 * s, x = fb->w - (npages - i) * (d + 2 * s) - s;
			gfx_fill(fb, x, (bar_h - d) / 2, d, d, i == page ? 0xFFFFFF : 0x6A98C8);
		}
	}
	const int cols = 6, cw = fb->w / cols;
	const int maxch = (cw - 8 * s) / (6 * s);
	for (int k = 0; k < 12; k++) {
		int x = (k % cols) * cw, y = top + (k / cols) * rh;
		int idx = page * 12 + k;
		char n[12], lab[24];
		snprintf(n, sizeof n, "%d", idx + 1);
		gfx_fill(fb, x + pad, y + pad / 2, cw - 2 * pad, rh - pad, cell);
		gfx_text(fb, x + 4 * s, y + 3 * s, n, s, num);
		snprintf(lab, sizeof lab, "%.*s", maxch > 0 && maxch < 23 ? maxch : 23, cfg->lcd_label[idx]);
		gfx_text(fb, x + 4 * s, y + rh / 2 + s, lab, s, fg);
	}
}

/* ---- LCDCfg.xml: what the LCD applets show for each button ---- */

#include <stdarg.h>
#include "output.h"

static const struct { const char *v3dk, *text; } button_names[] = {
	{"V3DK_MENU", "Menu"}, {"V3DK_FIT", "Fit"}, {"V3DK_TOP", "Top View"},
	{"V3DK_LEFT", "Left View"}, {"V3DK_RIGHT", "Right View"}, {"V3DK_FRONT", "Front View"},
	{"V3DK_BOTTOM", "Bottom View"}, {"V3DK_BACK", "Back View"}, {"V3DK_ROLL_CW", "Roll +"},
	{"V3DK_ROLL_CCW", "Roll -"}, {"V3DK_ISO1", "ISO1"}, {"V3DK_ISO2", "ISO2"},
	{"V3DK_ESC", "Esc"}, {"V3DK_ALT", "Alt"}, {"V3DK_SHIFT", "Shift"}, {"V3DK_CTRL", "Ctrl"},
	{"V3DK_ROTATE", "Rotation"}, {"V3DK_PANZOOM", "Pan Zoom"}, {"V3DK_DOMINANT", "Dominant"},
	{"V3DK_PLUS", "Plus"}, {"V3DK_MINUS", "Minus"},
};

static const char *const action_texts[TDX_ACT_COUNT] = {
	"", "Increase Speed", "Decrease Speed", "Dominant On/Off", "Rotation On/Off",
	"Pan Zoom On/Off", "Calibrate", "Next LCD Page", "Settings",
};

/* keyboard presets the settings panel offers, by evdev code chord */
static const struct { const char *text; int keys[3]; } key_presets[] = {
	{"Esc Key", {1}}, {"Ctrl Key", {29}}, {"Alt Key", {56}}, {"Shift Key", {42}},
	{"Enter Key", {28}}, {"Tab Key", {15}}, {"Delete Key", {111}}, {"Space Key", {57}},
	{"Copy", {29, 46}}, {"Cut", {29, 45}}, {"Paste", {29, 47}}, {"Undo", {29, 44}},
	{"Redo", {29, 42, 44}}, {"Save", {29, 31}}, {"Select All", {29, 30}}, {"Find", {29, 33}},
	{"Align Window Left", {125, 105}}, {"Align Window Right", {125, 106}},
	{"Cycle Apps", {56, 15}}, {"Show Desktop", {125, 32}},
	{"Redo", {29, 21}}, {"Task Switcher", {29, 56, 15}}, {"New", {29, 49}}, {"Open", {29, 24}},
};

static const char *key_short(int code)
{
	static const struct { int code; const char *n; } t[] = {
		{29, "Ctrl"}, {97, "Ctrl"}, {42, "Shift"}, {54, "Shift"}, {56, "Alt"}, {100, "AltGr"},
		{125, "Super"}, {126, "Super"}, {1, "Esc"}, {28, "Enter"}, {57, "Space"}, {15, "Tab"},
		{14, "Backspace"}, {111, "Delete"}, {110, "Insert"}, {102, "Home"}, {107, "End"},
		{104, "Page Up"}, {109, "Page Down"}, {103, "Up"}, {108, "Down"}, {105, "Left"},
		{106, "Right"},
	};
	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
		if (t[i].code == code)
			return t[i].n;
	return NULL;
}

static void key_name(int code, char *out, size_t len)
{
	static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
	const char *s = key_short(code);
	if (s)
		snprintf(out, len, "%s", s);
	else if (code >= 16 && code <= 25)
		snprintf(out, len, "%c", row1[code - 16]);
	else if (code >= 30 && code <= 38)
		snprintf(out, len, "%c", row2[code - 30]);
	else if (code >= 44 && code <= 50)
		snprintf(out, len, "%c", row3[code - 44]);
	else if (code >= 2 && code <= 11)
		snprintf(out, len, "%d", code == 11 ? 0 : code - 1);
	else if (code >= 59 && code <= 68)
		snprintf(out, len, "F%d", code - 58);
	else if (code == 87 || code == 88)
		snprintf(out, len, "F%d", code - 76);
	else
		snprintf(out, len, "Key %d", code);
}

void tdx_button_text(const struct tdx_config *cfg, const struct tdx_device_def *dev, int idx,
		     char *out, size_t len)
{
	const char *v3dk = (dev && idx < dev->nbuttons) ? dev->buttons[idx].v3dk : NULL;
	out[0] = 0;
	/* 0. the focused application's own name for the key (3dxlinuxctl app-buttons) */
	for (int i = 0; v3dk && i < cfg->napp_label; i++)
		if (!strcmp(cfg->app_label[i].v3dk, v3dk) && cfg->app_label[i].text[0]) {
			snprintf(out, len, "%s", cfg->app_label[i].text);
			return;
		}
	/* 1. explicit LCD label (lcd_label_N for V3DK_N, when set in the configuration) */
	if (v3dk && !strncmp(v3dk, "V3DK_", 5) && v3dk[5] >= '1' && v3dk[5] <= '9') {
		int n = atoi(v3dk + 5);
		char dflt[16];
		snprintf(dflt, sizeof dflt, "Key %d", n);
		if (n >= 1 && n <= TDX_MAX_LABELS && cfg->lcd_label[n - 1][0] &&
		    strcmp(cfg->lcd_label[n - 1], dflt)) {
			snprintf(out, len, "%s", cfg->lcd_label[n - 1]);
			return;
		}
	}
	/* 1b. named LCD label (lcd_label_V3DK_TOP = Top View) */
	for (int i = 0; v3dk && i < cfg->nnamed_label; i++)
		if (!strcmp(cfg->named_label[i].v3dk, v3dk) && cfg->named_label[i].text[0]) {
			snprintf(out, len, "%s", cfg->named_label[i].text);
			return;
		}
	/* 2. driver action */
	int act = tdx_action_for(cfg, dev, idx);
	if (act > TDX_ACT_NONE && act < TDX_ACT_COUNT) {
		snprintf(out, len, "%s", action_texts[act]);
		return;
	}
	/* 3. key binding */
	const struct tdx_keymap *km = tdx_keymap_for(cfg, dev, idx);
	if (km) {
		int n = 0;
		while (n < TDX_MAX_KEYS && km->keys[n])
			n++;
		for (size_t p = 0; !km->seq && p < sizeof key_presets / sizeof key_presets[0]; p++) {
			int m = 0;
			while (m < 3 && key_presets[p].keys[m])
				m++;
			if (m != n)
				continue;
			int same = 1;
			for (int k = 0; k < n; k++)
				same &= km->keys[k] == key_presets[p].keys[k];
			if (same) {
				snprintf(out, len, "%s", key_presets[p].text);
				return;
			}
		}
		size_t o = 0;
		for (int k = 0; k < n && o < len; k++) {
			char kn[24];
			key_name(km->keys[k], kn, sizeof kn);
			const char *sep = !k ? "" : km->step[k] != km->step[k - 1] ? ", " : "+";
			o += (size_t)snprintf(out + o, len - o, "%s%s", sep, kn);
		}
		return;
	}
	/* 4. the button's own function (sent to the application) */
	for (size_t i = 0; v3dk && i < sizeof button_names / sizeof button_names[0]; i++)
		if (!strcmp(v3dk, button_names[i].v3dk)) {
			snprintf(out, len, "%s", button_names[i].text);
			return;
		}
	if (v3dk && !strncmp(v3dk, "V3DK_", 5))
		snprintf(out, len, "Button %s", v3dk + 5);
	else
		snprintf(out, len, "Button %d", idx + 1);
}

static void xml_esc(char *out, size_t len, const char *s)
{
	size_t o = 0;
	for (; s && *s && o + 7 < len; s++) {
		const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;"
			      : *s == '"' ? "&quot;" : NULL;
		if (r)
			o += (size_t)snprintf(out + o, len - o, "%s", r);
		else
			out[o++] = *s;
	}
	out[o] = 0;
}

struct sbuf { char *b; size_t len, o; int full; };

static void sb_add(struct sbuf *s, const char *fmt, ...)
{
	if (s->full)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(s->b + s->o, s->len - s->o, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= s->len - s->o)
		s->full = 1;
	else
		s->o += (size_t)n;
}

int tdx_lcdcfg_xml(char *buf, size_t len, const struct tdx_config *cfg,
		   const struct tdx_device_def *dev, uint16_t vid, uint16_t pid,
		   const struct tdx_lcdcfg_info *info)
{
	struct sbuf s = {buf, len, 0, 0};
	char a[512], b[512];
	xml_esc(a, sizeof a, dev && dev->name ? dev->name : "");
	sb_add(&s, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
		   "<LCDCfg Default=\"false\" CfgFormatVersion=\"1.30\" ThisFileVersion=\"1.20\""
		   " Generator=\"3dxlinuxd\">\n<Device>\n\t<Name>%s</Name>\n\t<VendorID>%x</VendorID>\n"
		   "\t<ProductID>%x</ProductID>\n", a, vid, pid);
	sb_add(&s, "\t<Layout>\n\t\t<LCDIntensity>%d</LCDIntensity>\n\t\t<LEDIntensity>%d</LEDIntensity>\n"
		   "\t\t<ButtonStyle></ButtonStyle>\n\t\t<TextSize></TextSize>\n\t</Layout>\n",
	       info->lcd_level, info->led);
	sb_add(&s, "\t<Filters>\n\t\t<Rotation>%s</Rotation>\n\t\t<PanZoom>%s</PanZoom>\n"
		   "\t\t<Dominant>%s</Dominant>\n\t</Filters>\n",
	       cfg->disable_rotation ? "false" : "true", cfg->disable_translation ? "false" : "true",
	       cfg->dominant ? "true" : "false");
	xml_esc(a, sizeof a, info->app ? info->app : "");
	sb_add(&s, "\t<InfoLine>\n\t\t<Text>%s</Text>\n", a);
	if (info->slider >= 0)
		sb_add(&s, "\t\t<Slider>%d</Slider>\n", info->slider);
	else
		sb_add(&s, "\t\t<Slider/>\n");
	xml_esc(b, sizeof b, info->pressed ? info->pressed : "");
	if (b[0])
		sb_add(&s, "\t\t<ButtonPressed>%s</ButtonPressed>\n", b);
	else
		sb_add(&s, "\t\t<ButtonPressed/>\n");
	sb_add(&s, "\t\t<ApplicationName>%s</ApplicationName>\n", a);
	xml_esc(b, sizeof b, info->app_image ? info->app_image : "");
	sb_add(&s, "\t\t<Image>\n\t\t\t<Source>%s</Source>\n\t\t</Image>\n\t</InfoLine>\n", b);
	for (int i = 0; dev && i < dev->nbuttons; i++) {
		if (!dev->buttons[i].v3dk)
			continue;
		tdx_button_text(cfg, dev, i, b, sizeof b);
		xml_esc(a, sizeof a, b);
		sb_add(&s, "\t<Button>\n\t\t<V3DK>%s</V3DK>\n\t\t<Text>%s</Text>\n\t</Button>\n",
		       dev->buttons[i].v3dk, a);
	}
	sb_add(&s, "</Device>\n</LCDCfg>\n");
	return s.full ? -1 : (int)s.o;
}
