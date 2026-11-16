#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <linux/input-event-codes.h>

static const char *axis_names[6] = {"tx", "ty", "tz", "rx", "ry", "rz"};

void tdx_config_defaults(struct tdx_config *c)
{
	memset(c, 0, sizeof *c);
	c->sensitivity = 1.0;
	for (int i = 0; i < 6; i++) {
		c->axis_scale[i] = 1.0;
		c->axis_map[i] = i;
	}
	c->deadzone = 2;
	c->enable_uinput = 1;
	c->enable_spnav = 1;
	snprintf(c->spnav_socket, sizeof c->spnav_socket, "/run/spnav.sock");
	c->lcd_enable = 1;
	c->led = -1;
	c->led_button_color = -1;
	c->led_ring_color = -1;
	c->lcd_brightness = -1;
	for (int i = 0; i < 12; i++)
		snprintf(c->lcd_label[i], sizeof c->lcd_label[i], "Key %d", i + 1);
}

#define K(n) {#n, n}
static const struct { const char *name; int code; } keytab[] = {
	K(KEY_ESC), K(KEY_TAB), K(KEY_ENTER), K(KEY_SPACE), K(KEY_BACKSPACE), K(KEY_DELETE),
	K(KEY_INSERT), K(KEY_HOME), K(KEY_END), K(KEY_PAGEUP), K(KEY_PAGEDOWN),
	K(KEY_UP), K(KEY_DOWN), K(KEY_LEFT), K(KEY_RIGHT),
	K(KEY_LEFTCTRL), K(KEY_RIGHTCTRL), K(KEY_LEFTSHIFT), K(KEY_RIGHTSHIFT),
	K(KEY_LEFTALT), K(KEY_RIGHTALT), K(KEY_LEFTMETA), K(KEY_RIGHTMETA),
	K(KEY_A), K(KEY_B), K(KEY_C), K(KEY_D), K(KEY_E), K(KEY_F), K(KEY_G), K(KEY_H),
	K(KEY_I), K(KEY_J), K(KEY_K), K(KEY_L), K(KEY_M), K(KEY_N), K(KEY_O), K(KEY_P),
	K(KEY_Q), K(KEY_R), K(KEY_S), K(KEY_T), K(KEY_U), K(KEY_V), K(KEY_W), K(KEY_X),
	K(KEY_Y), K(KEY_Z),
	K(KEY_0), K(KEY_1), K(KEY_2), K(KEY_3), K(KEY_4), K(KEY_5), K(KEY_6), K(KEY_7),
	K(KEY_8), K(KEY_9),
	K(KEY_F1), K(KEY_F2), K(KEY_F3), K(KEY_F4), K(KEY_F5), K(KEY_F6), K(KEY_F7),
	K(KEY_F8), K(KEY_F9), K(KEY_F10), K(KEY_F11), K(KEY_F12), K(KEY_F13), K(KEY_F14),
	K(KEY_F15), K(KEY_F16), K(KEY_F17), K(KEY_F18), K(KEY_F19), K(KEY_F20),
	K(KEY_F21), K(KEY_F22), K(KEY_F23), K(KEY_F24),
	K(KEY_KP0), K(KEY_KP1), K(KEY_KP2), K(KEY_KP3), K(KEY_KP4), K(KEY_KP5),
	K(KEY_KP6), K(KEY_KP7), K(KEY_KP8), K(KEY_KP9), K(KEY_KPPLUS), K(KEY_KPMINUS),
	K(KEY_KPASTERISK), K(KEY_KPSLASH), K(KEY_KPENTER), K(KEY_KPDOT),
	K(KEY_MINUS), K(KEY_EQUAL), K(KEY_COMMA), K(KEY_DOT), K(KEY_SLASH),
	K(KEY_SEMICOLON), K(KEY_APOSTROPHE), K(KEY_LEFTBRACE), K(KEY_RIGHTBRACE),
	K(KEY_BACKSLASH), K(KEY_GRAVE), K(KEY_PRINT), K(KEY_PAUSE), K(KEY_MENU),
	K(KEY_VOLUMEUP), K(KEY_VOLUMEDOWN), K(KEY_MUTE), K(KEY_PLAYPAUSE),
	K(KEY_NEXTSONG), K(KEY_PREVIOUSSONG), K(KEY_UNDO), K(KEY_REDO),
	K(KEY_COPY), K(KEY_PASTE), K(KEY_CUT), K(KEY_FIND),
};
#undef K

int tdx_key_from_name(const char *name)
{
	for (size_t i = 0; i < sizeof keytab / sizeof keytab[0]; i++)
		if (!strcasecmp(keytab[i].name, name) || !strcasecmp(keytab[i].name + 4, name))
			return keytab[i].code;
	char *end;
	long v = strtol(name, &end, 0);
	return (*name && !*end && v > 0 && v < KEY_MAX) ? (int)v : -1;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static int axis_index(const char *s)
{
	for (int i = 0; i < 6; i++)
		if (!strcasecmp(s, axis_names[i]))
			return i;
	return -1;
}

/* "RRGGBB", "#RRGGBB" or "0xRRGGBB" -> 0xRRGGBB, -1 if malformed */
static int64_t parse_rgb(const char *v)
{
	if (*v == '#')
		v++;
	else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		v += 2;
	if (strlen(v) != 6)
		return -1;
	char *end;
	long x = strtol(v, &end, 16);
	return *end ? -1 : (int64_t)x;
}

static int parse_bool(const char *v) { return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || atoi(v) != 0; }

int tdx_config_load(struct tdx_config *c, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -errno;
	char line[512];
	int ln = 0;
	while (fgets(line, sizeof line, f)) {
		ln++;
		char *s = trim(line);
		if (!*s || *s == '#' || *s == ';')
			continue;
		char *eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "%s:%d: expected key = value\n", path, ln);
			continue;
		}
		*eq = 0;
		/* inline comment: whitespace, '#', then whitespace/EOL (so "#RRGGBB" survives) */
		for (char *h = eq + 1; *h; h++)
			if (*h == '#' && h > eq + 1 && isspace((unsigned char)h[-1]) &&
			    (!h[1] || isspace((unsigned char)h[1]))) {
				*h = 0;
				break;
			}
		char *k = trim(s), *v = trim(eq + 1);
		int ai;

		if (!strcmp(k, "sensitivity")) c->sensitivity = atof(v);
		else if (!strcmp(k, "deadzone")) c->deadzone = atoi(v);
		else if (!strcmp(k, "dominant")) c->dominant = parse_bool(v);
		else if (!strcmp(k, "disable_translation")) c->disable_translation = parse_bool(v);
		else if (!strcmp(k, "disable_rotation")) c->disable_rotation = parse_bool(v);
		else if (!strcmp(k, "uinput")) c->enable_uinput = parse_bool(v);
		else if (!strcmp(k, "spnav")) c->enable_spnav = parse_bool(v);
		else if (!strcmp(k, "spnav_socket")) snprintf(c->spnav_socket, sizeof c->spnav_socket, "%s", v);
		else if (!strcmp(k, "lcd")) c->lcd_enable = parse_bool(v);
		else if (!strcmp(k, "led")) c->led = parse_bool(v);
		else if (!strcmp(k, "led_button_color") || !strcmp(k, "led_ring_color") ||
			 !strncmp(k, "led_color_", 10)) {
			int64_t rgb = parse_rgb(v);
			if (rgb < 0)
				fprintf(stderr, "%s:%d: bad colour '%s' (want RRGGBB)\n", path, ln, v);
			else if (!strcmp(k, "led_button_color"))
				c->led_button_color = rgb;
			else if (!strcmp(k, "led_ring_color"))
				c->led_ring_color = rgb;
			else if (c->nledcolor < TDX_MAX_LEDCOLOR) {
				struct tdx_ledcolor *lc = &c->ledcolor[c->nledcolor++];
				snprintf(lc->v3dk, sizeof lc->v3dk, "%s", k + 10);
				lc->rgb = (uint32_t)rgb;
			}
		}
		else if (!strcmp(k, "lcd_brightness")) c->lcd_brightness = atoi(v);
		else if (!strcmp(k, "lcd_swap_rb")) c->lcd_swap_rb = parse_bool(v);
		else if (!strcmp(k, "lcd_image")) snprintf(c->lcd_image, sizeof c->lcd_image, "%s", v);
		else if (!strcmp(k, "verbose")) c->verbose = atoi(v);
		else if (!strncmp(k, "scale_", 6) && (ai = axis_index(k + 6)) >= 0) c->axis_scale[ai] = atof(v);
		else if (!strncmp(k, "invert_", 7) && (ai = axis_index(k + 7)) >= 0) {
			if (parse_bool(v)) c->axis_scale[ai] = -c->axis_scale[ai];
		} else if (!strncmp(k, "map_", 4) && (ai = axis_index(k + 4)) >= 0) {
			int src = axis_index(v);
			if (src >= 0) c->axis_map[ai] = src;
			else fprintf(stderr, "%s:%d: bad axis '%s'\n", path, ln, v);
		} else if (!strncmp(k, "lcd_label_", 10)) {
			int n = atoi(k + 10);
			if (n >= 1 && n <= 12)
				snprintf(c->lcd_label[n - 1], sizeof c->lcd_label[0], "%s", v);
		} else if (!strncmp(k, "key_", 4)) {
			if (c->nkeymap >= TDX_MAX_KEYMAP) continue;
			struct tdx_keymap *m = &c->keymap[c->nkeymap];
			memset(m, 0, sizeof *m);
			snprintf(m->v3dk, sizeof m->v3dk, "%s", k + 4);
			int nk = 0;
			for (char *tok = strtok(v, "+ "); tok && nk < 3; tok = strtok(NULL, "+ ")) {
				int code = tdx_key_from_name(tok);
				if (code < 0) fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, tok);
				else m->keys[nk++] = code;
			}
			if (nk) c->nkeymap++;
		} else
			fprintf(stderr, "%s:%d: unknown option '%s'\n", path, ln, k);
	}
	fclose(f);
	return 0;
}
