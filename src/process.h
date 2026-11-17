#ifndef TDX_PROCESS_H
#define TDX_PROCESS_H

#include <stdint.h>
#include "config.h"
#include "gfx.h"

/* Raw device axes -> output axes (map, scale, invert, deadzone, filters). */
void tdx_process_axes(const struct tdx_config *cfg, const int16_t raw[6], int out[6]);

/* Keymap lookup for logical button @idx of @dev (NULL if unmapped). */
const struct tdx_keymap *tdx_keymap_for(const struct tdx_config *cfg,
					const struct tdx_device_def *dev, int idx);

/* What button @idx does, as 3DxWare's LCDCfg.xml <Text> shows it: an explicit
 * lcd_label_N, else the driver action ("Increase Speed"), else the key binding
 * ("Copy", "Esc Key", "Ctrl+Shift+Z"), else the button's own function
 * ("Top View", "Fit", "Button 1"). */
void tdx_button_text(const struct tdx_config *cfg, const struct tdx_device_def *dev, int idx,
		     char *out, size_t len);

struct tdx_lcdcfg_info {
	const char *app;        /* focused application / profile name */
	const char *app_image;  /* its executable (icon source), may be NULL */
	const char *pressed;    /* text of the button just pressed, or NULL */
	int slider;             /* speed shown as a slider (percent), -1 = none */
	int lcd_level, led;     /* <LCDIntensity> 0..2, <LEDIntensity> 0/100 */
};
/* 3DxService's LCDCfg.xml (as read by the Function Keys applet) into @buf.
 * Returns the length, or -1 if it didn't fit. */
int tdx_lcdcfg_xml(char *buf, size_t len, const struct tdx_config *cfg,
		   const struct tdx_device_def *dev, uint16_t vid, uint16_t pid,
		   const struct tdx_lcdcfg_info *info);

/* Built-in action (action_<V3DK>) bound to logical button @idx, TDX_ACT_NONE if none. */
int tdx_action_for(const struct tdx_config *cfg, const struct tdx_device_def *dev, int idx);

/* Apply a toggle/speed action to @cfg (speed_up/down, dominant, rotation, panzoom).
 * Returns 1 if @cfg changed, 0 for actions handled elsewhere (calibrate, lcd_next). */
int tdx_apply_action(struct tdx_config *cfg, int action);

/*
 * Build the feature reports that apply cfg->led / led_*_color to an RGB-LED
 * device, following HIDDev.dll FUN_180033cd0:
 *   led = no                      -> one {.., 0x24, 0,0,0} (all off)
 *   all 30 button LEDs one colour -> one 0x25 broadcast
 *   otherwise                     -> one report per button LED 0..29
 *   ring colour set               -> one 0x26 broadcast
 * Unset colours default to white when led = yes. Returns number of reports.
 */
int tdx_rgb_plan(const struct tdx_config *cfg, const struct tdx_device_def *dev,
		 uint8_t out[][8], int max);

/* Render keys 1..12 as a 6x2 label grid, scaled to the panel (640x150 or 1280x416). */
void tdx_render_ent_page(struct tdx_fb *fb, const struct tdx_config *cfg, const char *devname,
			 int page, int npages);

/* Render the "function keys" page (labels 1..10) into a 320x240 fb. */
void tdx_render_keys_page(struct tdx_fb *fb, const struct tdx_config *cfg,
			  const char *devname, int page);

#endif
