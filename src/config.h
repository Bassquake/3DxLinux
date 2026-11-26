#ifndef TDX_CONFIG_H
#define TDX_CONFIG_H

#include "report.h"

#define TDX_MAX_KEYMAP 64
#define TDX_MAX_KEYS 32 /* keys in one binding, all its chords together */

struct tdx_keymap {
	char v3dk[32];  /* e.g. V3DK_ESC, or "BUTTON_7" for generic devices */
	int keys[TDX_MAX_KEYS + 1];          /* evdev KEY_* codes, 0-terminated */
	unsigned char step[TDX_MAX_KEYS + 1]; /* which chord each key belongs to */
	int seq;        /* 0: one chord held with the button ("KEY_LEFTCTRL + KEY_C");
			 * 1: chords tapped one after another on press ("KEY_V, KEY_F",
			 *    "KEY_LEFTALT + KEY_V, KEY_T": 3DxWare's keyboard macros) */
};

/* lcd_label_<V3DK id> = text: the LCD name of any key, e.g. a view key that a
 * profile binds to an application's own shortcut */
struct tdx_named_label {
	char v3dk[32];
	char text[24];
};
#define TDX_MAX_NAMED_LABELS 64

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
	TDX_ACT_SETTINGS,     /* "3Dconnexion Settings" (Driver_ShowDriverGUI): open tdx-settings */
	TDX_ACT_DISABLED,     /* "Disabled" (Driver_Disabled): the button does nothing */
	TDX_ACT_COMMONTOOLS,  /* "Common Tools" (Base_RadialMenu_CommonTools): 3DxWare's
			       * radial menu, shown by tdx-radial; a CadMouse's radial
			       * button does it by default */
	TDX_ACT_NUMPAD,       /* "Virtual NumPad" (Driver_Toggle_3DxNumPad): tdx-numpad */
	TDX_ACT_RADIAL,       /* "radial:ID": a radial menu of the user's own (the
			       * Radial Menu Editor's), its radial_ID_* lines */
	TDX_ACT_COUNT
};
#define TDX_MAX_ACTIONS 64
#define TDX_MAX_RADIAL_KV 192

struct tdx_button_action {
	char v3dk[32];
	int action;
	char menu[24]; /* TDX_ACT_RADIAL: the menu's ID */
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
	struct tdx_named_label named_label[TDX_MAX_NAMED_LABELS];
	int nnamed_label;
	/* the user's radial menus: radial_<ID>_name, _layout, _<n>, _<n>_label, _<n>_icon
	 * lines, kept as they are for tdx-radial ("radial ID" on the control socket) */
	struct tdx_kv { char key[48]; char val[160]; } radial[TDX_MAX_RADIAL_KV];
	int nradial;
	/* runtime, not from a file: names the focused application gave its keys
	 * (tdxctl app-buttons, e.g. the FreeCAD add-on); first in tdx_button_text */
	struct tdx_named_label app_label[TDX_MAX_NAMED_LABELS];
	int napp_label;
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
	/* a CadMouse on a Universal Receiver (3DxWare's Speed, Polling Rate and
	 * Smart Scrolling; sent to the mouse, see receiver.h) */
	int mouse_dpi;
	int mouse_polling_rate;
	int mouse_smart_scrolling;
	int mouse_swap_buttons;   /* "Swap Middle / Right Mouse Buttons" (per profile) */
	char calibrate_button[32];    /* V3DK id that re-zeroes the device */
	/* per-application profiles */
	char profile_dir[256];        /* directory of <name>.conf overlays */
	char control_socket[108];     /* tdxctl / tdx-focus command socket */
	char lcd_cfg[256];            /* LCDCfg.xml for the LCD applets ("" = off) */
	char match[256];              /* profile files: comma-separated process names */
};

void tdx_config_defaults(struct tdx_config *c);
/* Returns 0, -errno if unreadable (defaults kept). Unknown keys are warned and ignored. */
int tdx_config_load(struct tdx_config *c, const char *path);
/* Load a per-application overlay on top of @c. Daemon-wide options (sockets,
 * uinput, spnav, lcd on/off, profile_dir, lcd_bmp_dir, verbose) are ignored with a warning. */
int tdx_config_load_profile(struct tdx_config *c, const char *path);
/* A profile imported into a user's own folder: a plain file (no symlink), and
 * only match, key_*, action_* and lcd_label_* are taken. */
int tdx_config_load_user_profile(struct tdx_config *c, const char *path);
/* The desktop user's own settings (tdx-settings saves them, no password):
 * ~/.config/3dxlinux/3dxlinux.conf over /etc/3dxlinux.conf, and their own profiles. Read
 * only if they are plain files of @uid's, and without the options that are
 * paths or tdxd's own plumbing (uinput, sockets, folders ...). */
void tdx_config_set_user(int uid, const char *user_base);
int tdx_config_load_user_base(struct tdx_config *c);    /* 0 when there is none */
int tdx_config_load_own_profile(struct tdx_config *c, const char *path);
int tdx_key_from_name(const char *name);
/* Number of 12-label pages configured (1..4). */
int tdx_config_lcd_pages(const struct tdx_config *c);

#endif
