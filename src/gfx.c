#include "gfx.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* 5x7 font, '#' = pixel. Lowercase renders as uppercase. */
struct glyph { char c; const char *rows[7]; };
static const struct glyph font[] = {
	{' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
	{'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
	{'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
	{'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
	{'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
	{'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
	{'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
	{'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
	{'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
	{'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
	{'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
	{'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
	{'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
	{'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
	{'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
	{'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
	{'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
	{'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
	{'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
	{'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
	{'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
	{'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
	{'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
	{'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
	{'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
	{'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
	{'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
	{'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
	{'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
	{'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
	{'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
	{'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
	{'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
	{'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
	{'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
	{'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
	{'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
	{'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
	{',', {".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."}},
	{':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
	{'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
	{'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
	{'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
	{'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
	{'%', {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"}},
	{'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
	{')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
	{'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
	{'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
	{'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
	{'*', {".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", "....."}},
	{'#', {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."}},
	{'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."}},
	{'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
	{'\'', {"..#..", "..#..", ".#...", ".....", ".....", ".....", "....."}},
};

static const struct glyph *find_glyph(char c)
{
	c = (char)toupper((unsigned char)c);
	for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
		if (font[i].c == c)
			return &font[i];
	return c == '?' ? &font[0] : find_glyph('?');
}

void gfx_fill(struct tdx_fb *fb, int x, int y, int w, int h, uint32_t c)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < fb->h; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < fb->w; i++)
			fb->px[j * fb->w + i] = c;
}

int gfx_text_width(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

int gfx_text(struct tdx_fb *fb, int x, int y, const char *s, int scale, uint32_t c)
{
	for (; *s; s++, x += 6 * scale) {
		const struct glyph *g = find_glyph(*s);
		for (int r = 0; r < 7; r++)
			for (int k = 0; k < 5; k++)
				if (g->rows[r][k] == '#')
					gfx_fill(fb, x + k * scale, y + r * scale, scale, scale, c);
	}
	return x;
}

static int ppm_token(FILE *f, int *v)
{
	int ch;
	do {
		ch = fgetc(f);
		if (ch == '#')
			while (ch != '\n' && ch != EOF)
				ch = fgetc(f);
	} while (ch != EOF && isspace(ch));
	if (ch == EOF)
		return -1;
	ungetc(ch, f);
	return fscanf(f, "%d", v) == 1 ? 0 : -1;
}

int gfx_load_ppm(struct tdx_fb *fb, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -errno;
	char magic[3] = {0};
	int w, h, maxv, rc = -EINVAL;
	if (fread(magic, 1, 2, f) != 2 || strcmp(magic, "P6") ||
	    ppm_token(f, &w) || ppm_token(f, &h) || ppm_token(f, &maxv) ||
	    w <= 0 || h <= 0 || maxv <= 0 || maxv > 255)
		goto out;
	fgetc(f); /* single whitespace after maxval */
	gfx_fill(fb, 0, 0, fb->w, fb->h, 0);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int r = fgetc(f), g = fgetc(f), b = fgetc(f);
			if (b == EOF)
				goto out;
			if (x < fb->w && y < fb->h)
				fb->px[y * fb->w + x] = (uint32_t)((r * 255 / maxv) << 16 |
								   (g * 255 / maxv) << 8 |
								   (b * 255 / maxv));
		}
	}
	rc = 0;
out:
	fclose(f);
	return rc;
}

/* ---- anti-aliased text and RGBA images ---- */

#include <stdlib.h>

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
	if (a >= 255)
		return src & 0xFFFFFF;
	unsigned r = ((src >> 16 & 255) * a + (dst >> 16 & 255) * (255 - a)) / 255;
	unsigned g = ((src >> 8 & 255) * a + (dst >> 8 & 255) * (255 - a)) / 255;
	unsigned b = ((src & 255) * a + (dst & 255) * (255 - a)) / 255;
	return r << 16 | g << 8 | b;
}

int gfx_text_aa_width(const char *s, const struct gfx_font *f)
{
	int w = 0;
	for (; *s; s++)
		if ((unsigned char)*s >= 32 && (unsigned char)*s < 127)
			w += f->g[*s - 32].adv;
	return w;
}

int gfx_text_aa(struct tdx_fb *fb, int x, int y, const char *s, const struct gfx_font *f,
		uint32_t c)
{
	for (; *s; s++) {
		unsigned ch = (unsigned char)*s;
		if (ch < 32 || ch >= 127)
			continue;
		const struct gfx_glyph *g = &f->g[ch - 32];
		const uint8_t *a = f->alpha + g->off;
		for (int yy = 0; yy < g->h; yy++) {
			int py = y + g->y + yy;
			if (py < 0 || py >= fb->h)
				continue;
			for (int xx = 0; xx < g->w; xx++) {
				int px = x + g->x + xx;
				unsigned al = a[yy * g->w + xx];
				if (px < 0 || px >= fb->w || !al)
					continue;
				uint32_t *d = &fb->px[py * fb->w + px];
				*d = blend(*d, c, al);
			}
		}
		x += g->adv;
	}
	return x;
}

static int pam_header(FILE *f, int *w, int *h, int *depth)
{
	char line[128];
	*w = *h = *depth = 0;
	if (!fgets(line, sizeof line, f))
		return -EINVAL;
	if (!strncmp(line, "P6", 2)) { /* plain PPM is fine too */
		int maxv = 0;
		if (fscanf(f, "%d %d %d", w, h, &maxv) != 3 || maxv != 255)
			return -EINVAL;
		fgetc(f);
		*depth = 3;
		return 0;
	}
	if (strncmp(line, "P7", 2))
		return -EINVAL;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "ENDHDR", 6))
			return *w > 0 && *h > 0 && (*depth == 3 || *depth == 4) ? 0 : -EINVAL;
		sscanf(line, "WIDTH %d", w);
		sscanf(line, "HEIGHT %d", h);
		sscanf(line, "DEPTH %d", depth);
	}
	return -EINVAL;
}

int gfx_load_pam(struct gfx_img *img, const char *path)
{
	img->w = img->h = 0;
	img->rgba = NULL;
	FILE *f = fopen(path, "rb");
	if (!f)
		return -errno;
	int w, h, depth, rc = pam_header(f, &w, &h, &depth);
	if (rc || w > 4096 || h > 4096) {
		fclose(f);
		return rc ? rc : -EINVAL;
	}
	uint8_t *buf = malloc((size_t)w * h * 4);
	if (!buf) {
		fclose(f);
		return -ENOMEM;
	}
	for (int i = 0; i < w * h; i++) {
		uint8_t p[4] = {0, 0, 0, 255};
		if (fread(p, 1, (size_t)depth, f) != (size_t)depth) {
			free(buf);
			fclose(f);
			return -EINVAL;
		}
		memcpy(buf + i * 4, p, 4);
	}
	fclose(f);
	img->w = w;
	img->h = h;
	img->rgba = buf;
	return 0;
}

void gfx_img_free(struct gfx_img *img)
{
	free(img->rgba);
	img->rgba = NULL;
	img->w = img->h = 0;
}

void gfx_blit_part(struct tdx_fb *fb, const struct gfx_img *img, int sx, int sy, int sw, int sh,
		   int x, int y)
{
	if (!img || !img->rgba)
		return;
	for (int yy = 0; yy < sh; yy++) {
		int py = y + yy, iy = sy + yy;
		if (py < 0 || py >= fb->h || iy < 0 || iy >= img->h)
			continue;
		for (int xx = 0; xx < sw; xx++) {
			int px = x + xx, ix = sx + xx;
			if (px < 0 || px >= fb->w || ix < 0 || ix >= img->w)
				continue;
			const uint8_t *p = img->rgba + ((size_t)iy * img->w + ix) * 4;
			if (!p[3])
				continue;
			uint32_t *d = &fb->px[py * fb->w + px];
			*d = blend(*d, (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2], p[3]);
		}
	}
}

void gfx_blit(struct tdx_fb *fb, const struct gfx_img *img, int x, int y)
{
	if (img)
		gfx_blit_part(fb, img, 0, 0, img->w, img->h, x, y);
}

void gfx_vgradient(struct tdx_fb *fb, int x, int y, int w, int h, uint32_t c0, uint32_t c1)
{
	for (int yy = 0; yy < h; yy++) {
		unsigned a = h > 1 ? (unsigned)(yy * 255 / (h - 1)) : 0;
		gfx_fill(fb, x, y + yy, w, 1, blend(c0, c1, a));
	}
}
