#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
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
	c->mouse_dpi = 1400;
	c->mouse_polling_rate = 1000;
	c->mouse_smart_scrolling = 1;
	c->mouse_swap_buttons = 0;
	for (int i = 0; i < TDX_MAX_LABELS; i++)
		snprintf(c->lcd_label[i], sizeof c->lcd_label[i], "Key %d", i + 1);
	c->lcd_slide_step = 80; /* px per 25 ms step: a 1280 px slide takes 0.4 s */
	snprintf(c->profile_dir, sizeof c->profile_dir, "/etc/3dxlinux.d");
	snprintf(c->control_socket, sizeof c->control_socket, "/run/3dxlinux.sock");
	snprintf(c->lcd_cfg, sizeof c->lcd_cfg, "/run/3dxlinux/LCDCfg.xml");
}

static const char *const action_names[TDX_ACT_COUNT] = {
	"none", "speed_up", "speed_down", "dominant", "rotation", "panzoom", "calibrate", "lcd_next",
	"settings", "disabled", "commontools", "numpad", "radial",
};

const char *tdx_action_name(int action)
{
	return action > 0 && action < TDX_ACT_COUNT ? action_names[action] : "none";
}

int tdx_action_from_name(const char *name)
{
	if (!strncasecmp(name, "radial:", 7)) { /* radial:ID - letters, digits, _ */
		const char *id = name + 7;
		size_t n = strlen(id);
		if (!n || n >= sizeof ((struct tdx_button_action *)0)->menu)
			return -1;
		for (const char *p = id; *p; p++)
			if (!isalnum((unsigned char)*p) && *p != '_')
				return -1;
		return TDX_ACT_RADIAL;
	}
	if (!strcasecmp(name, "radial"))
		return -1;
	for (int i = 0; i < TDX_ACT_COUNT; i++)
		if (!strcasecmp(name, action_names[i]))
			return i;
	return -1;
}

int tdx_config_lcd_pages(const struct tdx_config *c)
{
	int p = (c->lcd_label_max + 11) / 12;
	return p < 1 ? 1 : p > TDX_MAX_LABELS / 12 ? TDX_MAX_LABELS / 12 : p;
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
	K(KEY_VOLUMEUP), K(KEY_VOLUMEDOWN), K(KEY_MUTE), K(KEY_MICMUTE), K(KEY_PLAYPAUSE),
	K(KEY_NEXTSONG), K(KEY_PREVIOUSSONG), K(KEY_UNDO), K(KEY_REDO),
	K(KEY_COPY), K(KEY_PASTE), K(KEY_CUT), K(KEY_FIND),
	K(KEY_CAPSLOCK), K(KEY_NUMLOCK), K(KEY_SCROLLLOCK), K(KEY_SYSRQ), K(KEY_102ND),
	K(KEY_COMPOSE), K(KEY_KPEQUAL), K(KEY_KPCOMMA),
	/* mouse buttons: a CadMouse button's mouse function (receiver.h); after the
	 * keys, so "LEFT" still means KEY_LEFT */
	K(BTN_LEFT), K(BTN_RIGHT), K(BTN_MIDDLE), K(BTN_SIDE), K(BTN_EXTRA),
};
#undef K

int tdx_key_from_name(const char *name)
{
	for (size_t i = 0; i < sizeof keytab / sizeof keytab[0]; i++)
		if (!strcasecmp(keytab[i].name, name) || !strcasecmp(keytab[i].name + 4, name))
			return keytab[i].code;
	/* a modifier without its side (3DxWare's "Ctrl+Shift+Z", the picker's
	 * Redo and Save As): the left one */
	static const struct { const char *name; int code; } mods[] = {
		{"SHIFT", KEY_LEFTSHIFT}, {"CTRL", KEY_LEFTCTRL}, {"CONTROL", KEY_LEFTCTRL},
		{"ALT", KEY_LEFTALT}, {"META", KEY_LEFTMETA}, {"SUPER", KEY_LEFTMETA},
		{"WIN", KEY_LEFTMETA}};
	const char *bare = !strncasecmp(name, "KEY_", 4) ? name + 4 : name;
	for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++)
		if (!strcasecmp(bare, mods[i].name))
			return mods[i].code;
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

/* Options that only make sense daemon-wide; ignored in per-app profiles. */
static int daemon_only(const char *k)
{
	static const char *const keys[] = {"uinput", "spnav", "spnav_socket", "lcd", "lcd_bmp_dir",
					   "verbose", "profile_dir", "control_socket", "lcd_cfg",
					   "calibrate_on_start", "mouse_dpi", "mouse_polling_rate",
					   "mouse_smart_scrolling", NULL};
	for (int i = 0; keys[i]; i++)
		if (!strcmp(k, keys[i]))
			return 1;
	return 0;
}

/* Find an existing entry for @name so later files override earlier ones. */
static struct tdx_keymap *keymap_slot(struct tdx_config *c, const char *name)
{
	for (int i = 0; i < c->nkeymap; i++)
		if (!strcasecmp(c->keymap[i].v3dk, name))
			return &c->keymap[i];
	return c->nkeymap < TDX_MAX_KEYMAP ? &c->keymap[c->nkeymap++] : NULL;
}

static struct tdx_ledcolor *ledcolor_slot(struct tdx_config *c, const char *name)
{
	for (int i = 0; i < c->nledcolor; i++)
		if (!strcasecmp(c->ledcolor[i].v3dk, name))
			return &c->ledcolor[i];
	return c->nledcolor < TDX_MAX_LEDCOLOR ? &c->ledcolor[c->nledcolor++] : NULL;
}

static int config_load(struct tdx_config *c, const char *path, int profile);

int tdx_config_load(struct tdx_config *c, const char *path) { return config_load(c, path, 0); }
int tdx_config_load_profile(struct tdx_config *c, const char *path) { return config_load(c, path, 1); }
int tdx_config_load_user_profile(struct tdx_config *c, const char *path) { return config_load(c, path, 2); }
int tdx_config_load_own_profile(struct tdx_config *c, const char *path) { return config_load(c, path, 4); }

/* modes: 0 the base (/etc), 1 a profile (/etc/3dxlinux.d), and the user's files:
 * 2 an imported 3DxWare profile (keys only), 3 their own settings over the
 * base, 4 their own profile */
static int user_uid = -1;
static char user_base[512];

void tdx_config_set_user(int uid, const char *base)
{
	user_uid = uid;
	snprintf(user_base, sizeof user_base, "%s", base ? base : "");
}

int tdx_config_load_user_base(struct tdx_config *c)
{
	if (!user_base[0])
		return 0;
	int rc = config_load(c, user_base, 3);
	return rc == -ENOENT ? 0 : rc;
}

/* Never from a user's file: what is a path 3dxlinuxd opens, or its own plumbing. */
static int user_denied(const char *k)
{
	static const char *const keys[] = {"uinput", "spnav", "spnav_socket", "control_socket",
					   "profile_dir", "lcd_cfg", "lcd_bmp_dir", "verbose", NULL};
	for (int i = 0; keys[i]; i++)
		if (!strcmp(k, keys[i]))
			return 1;
	return 0;
}

/* A user's picture for the LCD (lcd_image): a plain file of their own. */
static int user_image_ok(const char *path)
{
	struct stat st;
	return !lstat(path, &st) && S_ISREG(st.st_mode) &&
	       (user_uid < 0 || st.st_uid == (uid_t)user_uid);
}

/* Names set by the file being read, so a profile that rebinds a key drops
 * only the names earlier files gave it (the base config's "Save" on key 3). */
struct label_seen {
	char v3dk[TDX_MAX_NAMED_LABELS * 2][32];
	int n;
};

static void seen_add(struct label_seen *ls, const char *v3dk)
{
	if (ls->n < (int)(sizeof ls->v3dk / sizeof ls->v3dk[0]))
		snprintf(ls->v3dk[ls->n++], sizeof ls->v3dk[0], "%s", v3dk);
}

static int seen_has(const struct label_seen *ls, const char *v3dk)
{
	for (int i = 0; i < ls->n; i++)
		if (!strcasecmp(ls->v3dk[i], v3dk))
			return 1;
	return 0;
}

/* A profile gave @v3dk a new function: forget an earlier file's name for it. */
static void drop_old_label(struct tdx_config *c, const struct label_seen *ls, const char *v3dk)
{
	if (seen_has(ls, v3dk))
		return;
	for (int i = 0; i < c->nnamed_label; i++)
		if (!strcasecmp(c->named_label[i].v3dk, v3dk))
			c->named_label[i].text[0] = 0;
	int n = !strncasecmp(v3dk, "V3DK_", 5) && isdigit((unsigned char)v3dk[5]) ? atoi(v3dk + 5) : 0;
	if (n >= 1 && n <= TDX_MAX_LABELS)
		c->lcd_label[n - 1][0] = 0;
}

static int config_load(struct tdx_config *c, const char *path, int profile)
{
	static struct label_seen seen; /* config_load is not reentrant */
	seen.n = 0;
	FILE *f;
	if (profile >= 2) {
		/* a file from a user's own folder: no symlinks, only a plain file of
		 * theirs (3dxlinuxd reads it as root) */
		int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		struct stat st;
		if (fd < 0)
			return -errno;
		if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 256 * 1024 ||
		    (user_uid >= 0 && st.st_uid != (uid_t)user_uid)) {
			close(fd);
			return -EINVAL;
		}
		f = fdopen(fd, "r");
		if (!f) {
			close(fd);
			return -errno;
		}
	} else {
		f = fopen(path, "r");
	}
	if (!f)
		return -errno;
	char line[1024];
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

		if (profile == 2 && strcmp(k, "match") && strncmp(k, "key_", 4) &&
		    strncmp(k, "action_", 7) && strncmp(k, "lcd_label_", 10)) {
			/* imported for one user: keys, their names and actions only */
			fprintf(stderr, "%s:%d: option not allowed in an imported profile; ignored\n",
				path, ln);
			continue;
		}
		if (profile >= 2 && user_denied(k)) {
			fprintf(stderr, "%s:%d: '%s' only in /etc/3dxlinux.conf; ignored\n", path, ln, k);
			continue;
		}
		if (profile >= 2 && !strcmp(k, "lcd_image") && *v && !user_image_ok(v)) {
			fprintf(stderr, "%s:%d: lcd_image: not a file of your own; ignored\n", path, ln);
			continue;
		}
		if (profile && profile != 3 && daemon_only(k)) {
			fprintf(stderr, "%s:%d: '%s' is daemon-wide; ignored in a profile\n", path, ln, k);
			continue;
		}
		if (!strcmp(k, "match")) {
			snprintf(c->match, sizeof c->match, "%s", v);
			continue;
		}
		if (!strcmp(k, "sensitivity")) c->sensitivity = atof(v);
		else if (!strcmp(k, "calibrate_on_start")) c->calibrate_on_start = parse_bool(v);
		else if (!strcmp(k, "mouse_dpi")) {
			int d = atoi(v);
			c->mouse_dpi = d < 50 ? 50 : d > 8200 ? 8200 : d;
		} else if (!strcmp(k, "mouse_polling_rate")) {
			int r = atoi(v);
			c->mouse_polling_rate = r >= 1000 ? 1000 : r >= 500 ? 500 : r >= 250 ? 250 : 125;
		} else if (!strcmp(k, "mouse_smart_scrolling")) c->mouse_smart_scrolling = parse_bool(v);
		else if (!strcmp(k, "mouse_swap_buttons")) c->mouse_swap_buttons = parse_bool(v);
		else if (!strcmp(k, "calibrate_button")) snprintf(c->calibrate_button, sizeof c->calibrate_button, "%s", v);
		else if (!strcmp(k, "profile_dir")) snprintf(c->profile_dir, sizeof c->profile_dir, "%s", v);
		else if (!strcmp(k, "control_socket")) snprintf(c->control_socket, sizeof c->control_socket, "%s", v);
		else if (!strcmp(k, "lcd_cfg")) snprintf(c->lcd_cfg, sizeof c->lcd_cfg, "%s", strcasecmp(v, "none") ? v : "");
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
			else {
				struct tdx_ledcolor *lc = ledcolor_slot(c, k + 10);
				if (lc) {
					snprintf(lc->v3dk, sizeof lc->v3dk, "%s", k + 10);
					lc->rgb = (uint32_t)rgb;
				}
			}
		}
		else if (!strcmp(k, "lcd_brightness")) c->lcd_brightness = atoi(v);
		else if (!strcmp(k, "lcd_swap_rb")) c->lcd_swap_rb = parse_bool(v);
		else if (!strcmp(k, "lcd_image")) snprintf(c->lcd_image, sizeof c->lcd_image, "%s", v);
		else if (!strcmp(k, "verbose")) c->verbose = atoi(v);
		else if (!strncmp(k, "scale_", 6) && (ai = axis_index(k + 6)) >= 0) c->axis_scale[ai] = atof(v);
		else if (!strncmp(k, "invert_", 7) && (ai = axis_index(k + 7)) >= 0) {
			c->axis_invert[ai] = parse_bool(v); /* a setting, not a toggle: profiles can undo it */
		} else if (!strncmp(k, "map_", 4) && (ai = axis_index(k + 4)) >= 0) {
			int src = axis_index(v);
			if (src >= 0) c->axis_map[ai] = src;
			else fprintf(stderr, "%s:%d: bad axis '%s'\n", path, ln, v);
		} else if (!strcmp(k, "lcd_bmp_dir")) {
			snprintf(c->lcd_bmp_dir, sizeof c->lcd_bmp_dir, "%s", v);
		} else if (!strcmp(k, "lcd_slide_button")) {
			snprintf(c->lcd_slide_button, sizeof c->lcd_slide_button, "%s", v);
		} else if (!strcmp(k, "lcd_slide_step")) {
			c->lcd_slide_step = atoi(v) > 0 ? atoi(v) : c->lcd_slide_step;
		} else if (!strncmp(k, "lcd_label_V3DK_", 15)) {
			/* named: lcd_label_V3DK_TOP = Top View (later files override) */
			struct tdx_named_label *l = NULL;
			for (int i = 0; i < c->nnamed_label; i++)
				if (!strcmp(c->named_label[i].v3dk, k + 10))
					l = &c->named_label[i];
			if (!l && c->nnamed_label < TDX_MAX_NAMED_LABELS)
				l = &c->named_label[c->nnamed_label++];
			if (l) {
				snprintf(l->v3dk, sizeof l->v3dk, "%s", k + 10);
				snprintf(l->text, sizeof l->text, "%s", v);
			}
			seen_add(&seen, k + 10);
			/* lcd_label_V3DK_3 and lcd_label_3 name the same key: the later
			 * one wins (a profile's name over the base config's number) */
			int n = isdigit((unsigned char)k[15]) ? atoi(k + 15) : 0;
			if (n >= 1 && n <= TDX_MAX_LABELS)
				c->lcd_label[n - 1][0] = 0;
		} else if (!strncmp(k, "lcd_label_", 10)) {
			int n = atoi(k + 10);
			if (n >= 1 && n <= TDX_MAX_LABELS) {
				char named[32];
				snprintf(named, sizeof named, "V3DK_%d", n);
				for (int i = 0; i < c->nnamed_label; i++)
					if (!strcmp(c->named_label[i].v3dk, named))
						c->named_label[i].text[0] = 0;
				seen_add(&seen, named);
				snprintf(c->lcd_label[n - 1], sizeof c->lcd_label[0], "%s", v);
				if (n > c->lcd_label_max)
					c->lcd_label_max = n;
			}
		} else if (!strncmp(k, "action_", 7)) {
			int act = tdx_action_from_name(v);
			if (act < 0) {
				fprintf(stderr, "%s:%d: unknown action '%s'\n", path, ln, v);
				continue;
			}
			struct tdx_button_action *a = NULL;
			for (int i = 0; i < c->nactions; i++)
				if (!strcasecmp(c->actions[i].v3dk, k + 7))
					a = &c->actions[i];
			if (!a && c->nactions < TDX_MAX_ACTIONS)
				a = &c->actions[c->nactions++];
			if (a) {
				snprintf(a->v3dk, sizeof a->v3dk, "%s", k + 7);
				if (profile)
					drop_old_label(c, &seen, k + 7);
				a->action = act; /* "none" (0) clears a base action */
				snprintf(a->menu, sizeof a->menu, "%s",
					 act == TDX_ACT_RADIAL ? v + 7 : "");
			}
		} else if (!strncmp(k, "key_", 4)) {
			/* later files override earlier ones; "none" removes the binding.
			 * "A + B" is a chord held with the button, "A, B" a sequence
			 * tapped on press (e.g. FreeCAD's Fit All: V, F) */
			int keys[TDX_MAX_KEYS + 1] = {0}, nk = 0, ns = 0, bad = 0;
			unsigned char step[TDX_MAX_KEYS + 1] = {0};
			if (strcasecmp(v, "none")) {
				char *csave = NULL;
				for (char *ch = strtok_r(v, ",", &csave); ch; ch = strtok_r(NULL, ",", &csave)) {
					int before = nk;
					char *ksave = NULL;
					for (char *tok = strtok_r(ch, "+ \t", &ksave); tok;
					     tok = strtok_r(NULL, "+ \t", &ksave)) {
						int code = tdx_key_from_name(tok);
						if (code < 0) {
							fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, tok);
							bad = 1;
						} else if (nk < TDX_MAX_KEYS && ns < 255) {
							keys[nk] = code;
							step[nk++] = (unsigned char)ns;
						}
					}
					if (nk > before)
						ns++;
				}
			}
			if (bad && !nk)
				continue;
			struct tdx_keymap *m = keymap_slot(c, k + 4);
			if (!m)
				continue;
			memset(m, 0, sizeof *m);
			snprintf(m->v3dk, sizeof m->v3dk, "%s", k + 4);
			if (profile)
				drop_old_label(c, &seen, k + 4);
			memcpy(m->keys, keys, sizeof keys);
			memcpy(m->step, step, sizeof step);
			m->seq = ns > 1;
		} else if (!strcmp(k, "mouse_osd")) {
			/* 3dxlinux-osd's (the On-Screen Display): nothing for 3dxlinuxd to do */
		} else if (!strncmp(k, "radial_", 7) && strlen(k) < sizeof c->radial[0].key) {
			/* a radial menu's line: later files (the profile) override */
			struct tdx_kv *kv = NULL;
			for (int i = 0; i < c->nradial && !kv; i++)
				if (!strcmp(c->radial[i].key, k))
					kv = &c->radial[i];
			if (!kv && c->nradial < TDX_MAX_RADIAL_KV)
				kv = &c->radial[c->nradial++];
			if (kv) {
				snprintf(kv->key, sizeof kv->key, "%s", k);
				snprintf(kv->val, sizeof kv->val, "%s", v);
			}
		} else
			fprintf(stderr, "%s:%d: unknown option '%s'\n", path, ln, k);
	}
	fclose(f);
	return 0;
}
