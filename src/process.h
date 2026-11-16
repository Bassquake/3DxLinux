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

/* Render the Enterprise 640x150 page: labels for keys 1..12 in a 6x2 grid. */
void tdx_render_ent_page(struct tdx_fb *fb, const struct tdx_config *cfg, const char *devname);

/* Render the "function keys" page (labels 1..10) into a 320x240 fb. */
void tdx_render_keys_page(struct tdx_fb *fb, const struct tdx_config *cfg,
			  const char *devname, int page);

#endif
