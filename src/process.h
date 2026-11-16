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

/* Render the "function keys" page (labels 1..10) into a 320x240 fb. */
void tdx_render_keys_page(struct tdx_fb *fb, const struct tdx_config *cfg,
			  const char *devname, int page);

#endif
