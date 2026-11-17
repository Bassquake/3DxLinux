#ifndef TDX_CONFIG_H
#define TDX_CONFIG_H

#include "report.h"

#define TDX_MAX_KEYMAP 64

struct tdx_keymap {
	char v3dk[32];  /* e.g. V3DK_ESC, or "BUTTON_7" for generic devices */
	int keys[4];    /* evdev KEY_* codes, chord, 0-terminated */
};

/* Built-in button actions (3DxWare's driver actions), set with action_<V3DK> = NAME */
enum tdx_action {
	TDX_ACT_NONE = 0,
	TDX_ACT_SPEED_UP,     /* "Increase Speed"   */
	TDX_ACT_SPEED_DOWN,   /* "Decrease Speed"   */
	TDX_ACT_DOMINANT,     /* "Dominant On/Off"  */
	TDX_ACT_ROTATION,     /* "Rotation On/Off"  */
	TDX_ACT_PANZOOM,      /* "Pan Zoom On/Off"  */
	TDX_ACT_CALIBRATE,    /* "Calibrate" (Driver_RezeroDevice) */
	TDX_ACT_LCD_NEXT,     /* Giotto: slide to the next LCD page */
	TDX_ACT_COUNT
};
#define TDX_MAX_ACTIONS 64

struct tdx_button_action {
	char v3dk[32];
	int action;
};

const char *tdx_action_name(int action);
int tdx_action_from_name(const char *name);

#define TDX_MAX_LEDCOLOR 64
#define TDX_MAX_LABELS 48 /* 4 Giotto slide pages x 12 keys */

struct tdx_ledcolor {
	char v3dk[32];  /* button whose LED this colours, e.g. V3DK_FIT, or "LED_n" */
	uint32_t rgb;   /* 0xRRGGBB */
};

struct tdx_config {
	double sensitivity;       /* global multiplier */
	double axis_scale[6];     /* per axis multiplier (0 = axis off, like Windows "Active") */
	int axis_invert[6];       /* per axis reverse (Windows "Reverse") */
	int axis_map[6];          /* output axis i <- input axis axis_map[i] */
	int deadzone;             /* raw units */
	int disable_translation, disable_rotation, dominant;
	int enable_uinput;
	int enable_spnav;
	char spnav_socket[108];
	int lcd_enable;
	int led;                  /* -1 keep, 0 off, 1 on (HID output report 0x04) */
	/* RGB LEDs (Giotto, feature report 0x1A): -1 = not set */
	int64_t led_button_color; /* all button LEDs */
	int64_t led_ring_color;   /* the 6 ring segments */
	struct tdx_ledcolor ledcolor[TDX_MAX_LEDCOLOR];
	int nledcolor;
	int lcd_brightness;       /* 0..100, -1 = keep */
	char lcd_label[TDX_MAX_LABELS][24]; /* key labels; SPP 1..10, Enterprise 1..12,
					     * Giotto 12 per slide page (up to 4 pages) */
	int lcd_label_max;        /* highest lcd_label_N set in the file */
	char lcd_slide_button[32];/* V3DK id that slides the Giotto LCD to the next page */
	char lcd_bmp_dir[256];    /* Giotto legacy path: watch DIR/sp4LCD.bmp (0x13) */
	int lcd_slide_step;       /* slide speed, pixels per 25 ms step */
	int lcd_swap_rb;          /* Enterprise LCD: input channel order (inferred, see docs) */
	char lcd_image[256];      /* optional 320x240 PPM shown instead of labels */
	struct tdx_keymap keymap[TDX_MAX_KEYMAP];
	int nkeymap;
	struct tdx_button_action actions[TDX_MAX_ACTIONS];
	int nactions;
	int verbose;
	/* calibration (HIDDev.dll UdDevRezero: feature report {0x07, 0x00}) */
	int calibrate_on_start;
	char calibrate_button[32];    /* V3DK id that re-zeroes the device */
	/* per-application profiles */
	char profile_dir[256];        /* directory of <name>.conf overlays */
	char control_socket[108];     /* tdxctl / tdx-focus command socket */
	char match[256];              /* profile files: comma-separated process names */
};

void tdx_config_defaults(struct tdx_config *c);
/* Returns 0, -errno if unreadable (defaults kept). Unknown keys are warned and ignored. */
int tdx_config_load(struct tdx_config *c, const char *path);
/* Load a per-application overlay on top of @c. Daemon-wide options (sockets,
 * uinput, spnav, lcd on/off, profile_dir, lcd_bmp_dir, verbose) are ignored with a warning. */
int tdx_config_load_profile(struct tdx_config *c, const char *path);
int tdx_key_from_name(const char *name);
/* Number of 12-label pages configured (1..4). */
int tdx_config_lcd_pages(const struct tdx_config *c);

#endif
