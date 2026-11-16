#ifndef TDX_CONFIG_H
#define TDX_CONFIG_H

#include "report.h"

#define TDX_MAX_KEYMAP 64

struct tdx_keymap {
	char v3dk[32];  /* e.g. V3DK_ESC, or "BUTTON_7" for generic devices */
	int keys[4];    /* evdev KEY_* codes, chord, 0-terminated */
};

struct tdx_config {
	double sensitivity;       /* global multiplier */
	double axis_scale[6];     /* per axis multiplier, sign = invert */
	int axis_map[6];          /* output axis i <- input axis axis_map[i] */
	int deadzone;             /* raw units */
	int disable_translation, disable_rotation, dominant;
	int enable_uinput;
	int enable_spnav;
	char spnav_socket[108];
	int lcd_enable;
	int led;                  /* -1 keep, 0 off, 1 on (HID output report 0x04) */
	int lcd_brightness;       /* 0..100, -1 = keep */
	char lcd_label[10][24];   /* function keys 1..10 */
	char lcd_image[256];      /* optional 320x240 PPM shown instead of labels */
	struct tdx_keymap keymap[TDX_MAX_KEYMAP];
	int nkeymap;
	int verbose;
};

void tdx_config_defaults(struct tdx_config *c);
/* Returns 0, -errno if unreadable (defaults kept). Unknown keys are warned and ignored. */
int tdx_config_load(struct tdx_config *c, const char *path);
int tdx_key_from_name(const char *name);

#endif
