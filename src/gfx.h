/* Minimal 32bpp drawing for the 320x240 LCD (no external deps). */
#ifndef TDX_GFX_H
#define TDX_GFX_H

#include <stdint.h>

struct tdx_fb {
	int w, h;
	uint32_t *px; /* 0x00RRGGBB, row-major */
};

void gfx_fill(struct tdx_fb *fb, int x, int y, int w, int h, uint32_t c);
/* Draw text with the built-in 5x7 font scaled by @scale. Returns end x. */
int gfx_text(struct tdx_fb *fb, int x, int y, const char *s, int scale, uint32_t c);
int gfx_text_width(const char *s, int scale);

/* Load binary PPM (P6, maxval<=255) into a 320x240 fb (cropped / padded). */
int gfx_load_ppm(struct tdx_fb *fb, const char *path);

#endif
