#define _GNU_SOURCE
#include "lcd_giotto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deflate.h"
#include "usbfs.h"

static void cmd_init(uint8_t pkt[TDX_GIO_PKT], uint8_t op)
{
	memset(pkt, 0, TDX_GIO_PKT);
	pkt[0] = op;
	pkt[22] = 0x55; /* 0xaa55 at bytes 22..23 (u64 0xaa55000000000000 at offset 16) */
	pkt[23] = 0xAA;
}

void tdx_gio_cmd_write(uint8_t pkt[TDX_GIO_PKT], uint8_t buf, uint32_t len)
{
	cmd_init(pkt, TDX_GIO_OP_WRITE); /* FUN_180048530 */
	pkt[2] = buf;
	pkt[3] = 1;
	pkt[4] = (uint8_t)len;
	pkt[5] = (uint8_t)(len >> 8);
	pkt[6] = (uint8_t)(len >> 16);
	pkt[7] = (uint8_t)(len >> 24);
}

void tdx_gio_cmd_copy(uint8_t pkt[TDX_GIO_PKT], uint8_t b1, uint8_t b2)
{
	cmd_init(pkt, TDX_GIO_OP_COPY); /* FUN_180048890(dev, p2, p3): [1]=p3, [2]=p2 */
	pkt[1] = b1;
	pkt[2] = b2;
}

void tdx_gio_cmd_show(uint8_t pkt[TDX_GIO_PKT], uint8_t buf)
{
	cmd_init(pkt, TDX_GIO_OP_SHOW); /* FUN_180048980 */
	pkt[1] = buf;
}

void tdx_gio_pack(const uint32_t *xrgb, uint8_t *out, int swap_rb)
{
	/* FUN_18004a760, rotate=1, full frame (x=y=0, w=1280, h=416):
	 * out[((1279 - x) * 416 + y) * 3 + k] = memory byte k of the 32 bpp pixel.
	 * On Windows the 32 bpp source is B,G,R,X in memory. */
	for (int y = 0; y < TDX_GIO_H; y++) {
		const uint32_t *row = xrgb + (size_t)y * TDX_GIO_W;
		for (int x = 0; x < TDX_GIO_W; x++) {
			uint32_t p = row[x];
			uint8_t r = (uint8_t)(p >> 16), g = (uint8_t)(p >> 8), b = (uint8_t)p;
			uint8_t *o = out + ((size_t)(TDX_GIO_W - 1 - x) * TDX_GIO_H + y) * 3;
			o[0] = swap_rb ? r : b;
			o[1] = g;
			o[2] = swap_rb ? b : r;
		}
	}
}

static int send_cmd(struct tdx_giolcd *l, const uint8_t pkt[TDX_GIO_PKT])
{
	return tdx_usb_bulk_out(l->fd, l->ep_out, pkt, TDX_GIO_PKT, 1000);
}

int tdx_giolcd_open(struct tdx_giolcd *l, uint16_t vid, uint16_t pid)
{
	l->fd = -1;
	l->iface = 0;
	l->st = (struct tdx_gio_state){.a = 1, .b = 0, .c = 3, .d = 2, /* FUN_180048fb0 */
				       .page_in_c = -1, .page_in_d = -1};
	l->npages = 0;
	int rc = tdx_usb_find_node(vid, pid, l->path, sizeof l->path);
	if (rc)
		return rc;
	/* The WinUSB function is the vendor-class interface; find it rather than assume 0. */
	int ifc;
	if (!tdx_usb_find_vendor_iface(l->path, &ifc))
		l->iface = ifc;
	int fd = tdx_usb_claim(l->path, l->iface);
	if (fd < 0)
		return fd;
	l->fd = fd;
	if (tdx_usb_find_bulk_out(fd, l->iface, &l->ep_out))
		l->ep_out = 0x01;
	uint8_t pkt[TDX_GIO_PKT];
	tdx_gio_cmd_show(pkt, (uint8_t)l->st.b); /* init: show the front buffer */
	rc = send_cmd(l, pkt);
	if (rc) {
		tdx_giolcd_close(l);
		return rc;
	}
	return 0;
}

void tdx_giolcd_close(struct tdx_giolcd *l)
{
	tdx_usb_release(l->fd, l->iface);
	l->fd = -1;
}

/* Compress an image into l->comp. Returns length or -EFBIG. */
static long compress_image(struct tdx_giolcd *l, const uint32_t *xrgb)
{
	tdx_gio_pack(xrgb, l->raw, l->swap_rb);
	long n = tdx_deflate_fixed(l->raw, sizeof l->raw, l->comp, sizeof l->comp);
	return (n < 0 || n > TDX_GIO_COMP_MAX) ? -EFBIG : n;
}

/* 0x20 {buf, len} + payload, without any buffer flip (FUN_18004aee0 with buf != A/B). */
static int upload(struct tdx_giolcd *l, int buf, const uint32_t *xrgb)
{
	long n = compress_image(l, xrgb);
	if (n < 0)
		return (int)n;
	uint8_t pkt[TDX_GIO_PKT];
	tdx_gio_cmd_write(pkt, (uint8_t)buf, (uint32_t)n);
	int rc = send_cmd(l, pkt);
	return rc ? rc : tdx_usb_bulk_out(l->fd, l->ep_out, l->comp, (size_t)n, 5000);
}

int tdx_giolcd_show(struct tdx_giolcd *l, const uint32_t *xrgb)
{
	if (l->fd < 0)
		return -EBADF;
	long n = compress_image(l, xrgb);
	if (n < 0)
		return (int)n;

	uint8_t pkts[4][TDX_GIO_PKT];
	int rc;
	tdx_gio_plan_frame(&l->st.a, &l->st.b, l->st.c, (uint32_t)n, pkts);
	l->st.page_in_c = -1; /* 0x22 {B, C} overwrites the spare buffer */
	if ((rc = send_cmd(l, pkts[0])) ||
	    (rc = tdx_usb_bulk_out(l->fd, l->ep_out, l->comp, (size_t)n, 5000)))
		return rc;
	for (int i = 1; i < 4; i++)
		if ((rc = send_cmd(l, pkts[i])))
			return rc;
	return 0;
}

int tdx_giolcd_set_pages(struct tdx_giolcd *l, const uint32_t *const *pages, int n)
{
	if (n < 1 || n > TDX_GIO_MAX_PAGES)
		return -EINVAL;
	for (int i = 0; i < n; i++)
		l->pages[i] = pages[i];
	l->npages = n;
	l->st.page_in_c = l->st.page_in_d = -1; /* contents changed: re-upload */
	return 0;
}

int tdx_giolcd_slide(struct tdx_giolcd *l, int x)
{
	if (l->fd < 0)
		return -EBADF;
	if (!l->npages)
		return -EINVAL;
	struct tdx_gio_step steps[8];
	int n = tdx_gio_plan_slide(&l->st, x, l->npages, steps, 8);
	if (n < 0)
		return -EINVAL;
	for (int i = 0; i < n; i++) {
		int rc = steps[i].kind == TDX_GIO_STEP_UPLOAD
				 ? upload(l, steps[i].buf, l->pages[steps[i].page])
				 : send_cmd(l, steps[i].pkt);
		if (rc) {
			l->st.page_in_c = l->st.page_in_d = -1;
			return rc;
		}
	}
	return 0;
}

void tdx_gio_cmd_copy_rect(uint8_t pkt[TDX_GIO_PKT], uint8_t src, uint8_t dst, uint16_t sy,
			   uint16_t smx, uint16_t dy, uint16_t dmx, uint16_t h, uint16_t w)
{
	cmd_init(pkt, TDX_GIO_OP_COPY_RECT); /* FUN_180048750(dev, dst, src, sy, smx, h, w, dy, dmx) */
	pkt[1] = src;
	pkt[2] = dst;
	const uint16_t f[6] = {sy, smx, dy, dmx, h, w};
	for (int i = 0; i < 6; i++) {
		pkt[8 + 2 * i] = (uint8_t)f[i];
		pkt[9 + 2 * i] = (uint8_t)(f[i] >> 8);
	}
}

static int add_cmd(struct tdx_gio_step *steps, int *n, int max)
{
	if (*n >= max)
		return -1;
	steps[*n].kind = TDX_GIO_STEP_CMD;
	return (*n)++;
}

int tdx_gio_plan_slide(struct tdx_gio_state *s, int x, int npages, struct tdx_gio_step *steps,
		       int max)
{
	if (npages < 1 || npages > TDX_GIO_MAX_PAGES)
		return -1;
	int last = npages - 1;
	if (x < 0)
		x = 0;
	if (x > last * TDX_GIO_W)
		x = last * TDX_GIO_W;
	int page = x / TDX_GIO_W, off = x % TDX_GIO_W;
	int n = 0, k;

	if (page < last) {
		/* D <- page, C <- page + 1 (uploaded only when they changed) */
		if (s->page_in_d != page) {
			if (n >= max) return -1;
			steps[n++] = (struct tdx_gio_step){.kind = TDX_GIO_STEP_UPLOAD, .buf = s->d, .page = page};
			s->page_in_d = page;
		}
		if (off && s->page_in_c != page + 1) {
			if (n >= max) return -1;
			steps[n++] = (struct tdx_gio_step){.kind = TDX_GIO_STEP_UPLOAD, .buf = s->c, .page = page + 1};
			s->page_in_c = page + 1;
		}
		if ((k = add_cmd(steps, &n, max)) < 0) return -1;
		tdx_gio_cmd_copy_rect(steps[k].pkt, (uint8_t)s->d, (uint8_t)s->a, TDX_GIO_SLIDE_Y, 0,
				      TDX_GIO_SLIDE_Y, (uint16_t)off, TDX_GIO_SLIDE_H,
				      (uint16_t)(TDX_GIO_W - off));
		if (off) {
			if ((k = add_cmd(steps, &n, max)) < 0) return -1;
			tdx_gio_cmd_copy_rect(steps[k].pkt, (uint8_t)s->c, (uint8_t)s->a, TDX_GIO_SLIDE_Y,
					      (uint16_t)(TDX_GIO_W - off), TDX_GIO_SLIDE_Y, 0,
					      TDX_GIO_SLIDE_H, (uint16_t)off);
		}
	} else {
		/* last page: copied whole from C */
		if (s->page_in_c != page) {
			if (n >= max) return -1;
			steps[n++] = (struct tdx_gio_step){.kind = TDX_GIO_STEP_UPLOAD, .buf = s->c, .page = page};
			s->page_in_c = page;
		}
		if ((k = add_cmd(steps, &n, max)) < 0) return -1;
		tdx_gio_cmd_copy_rect(steps[k].pkt, (uint8_t)s->c, (uint8_t)s->a, TDX_GIO_SLIDE_Y, 0,
				      TDX_GIO_SLIDE_Y, 0, TDX_GIO_SLIDE_H, TDX_GIO_W);
	}
	/* swap A <-> B and show the composed buffer */
	int t = s->b;
	s->b = s->a;
	s->a = t;
	if ((k = add_cmd(steps, &n, max)) < 0) return -1;
	tdx_gio_cmd_show(steps[k].pkt, (uint8_t)s->b);
	return n;
}

void tdx_gio_plan_frame(int *buf_a, int *buf_b, int buf_c, uint32_t comp_len,
			uint8_t pkts[4][TDX_GIO_PKT])
{
	/* FUN_18004aee0 with usage 0 (static image) */
	tdx_gio_cmd_write(pkts[0], (uint8_t)*buf_a, comp_len); /* 0x20 + payload follows */
	int t = *buf_b;                                         /* flip A <-> B */
	*buf_b = *buf_a;
	*buf_a = t;
	tdx_gio_cmd_show(pkts[1], (uint8_t)*buf_b);              /* 0x24 {front} */
	tdx_gio_cmd_copy(pkts[2], (uint8_t)*buf_b, (uint8_t)*buf_a); /* 0x22 {front, back} */
	tdx_gio_cmd_copy(pkts[3], (uint8_t)*buf_b, (uint8_t)buf_c);  /* 0x22 {front, spare} */
}

void tdx_gio_pack_rect(const uint32_t *xrgb, int x, int y, int w, int h, uint8_t *out,
		       int swap_rb)
{
	/* FUN_18004a760, rotate=1: out[((w-1-i)*h + (row-y))*3 + k] */
	for (int j = 0; j < h; j++) {
		const uint32_t *row = xrgb + (size_t)(y + j) * TDX_GIO_W + x;
		for (int i = 0; i < w; i++) {
			uint32_t p = row[i];
			uint8_t r = (uint8_t)(p >> 16), g = (uint8_t)(p >> 8), b = (uint8_t)p;
			uint8_t *o = out + ((size_t)(w - 1 - i) * h + j) * 3;
			o[0] = swap_rb ? r : b;
			o[1] = g;
			o[2] = swap_rb ? b : r;
		}
	}
}

void tdx_gio_cmd_write_rect(uint8_t pkt[TDX_GIO_PKT], uint8_t buf, uint32_t len, uint16_t y,
			    uint16_t mx, uint16_t h, uint16_t w)
{
	cmd_init(pkt, TDX_GIO_OP_WRITE_RECT); /* FUN_180048630 */
	pkt[2] = buf;
	pkt[3] = 1;
	pkt[4] = (uint8_t)len;
	pkt[5] = (uint8_t)(len >> 8);
	pkt[6] = (uint8_t)(len >> 16);
	pkt[7] = (uint8_t)(len >> 24);
	const uint16_t f[4] = {y, mx, h, w}; /* bytes 8..11 stay zero */
	for (int i = 0; i < 4; i++) {
		pkt[12 + 2 * i] = (uint8_t)f[i];
		pkt[13 + 2 * i] = (uint8_t)(f[i] >> 8);
	}
}

uint32_t tdx_gio_comp_max(int w, int h)
{
	uint32_t raw = (uint32_t)w * (uint32_t)h * 3;
	uint32_t a = raw / 0x7c00 * 5 + raw + 0x85;
	uint32_t b = (uint32_t)w * (uint32_t)h * 0x14a / 100 + 0x80;
	return a > b ? a : b;
}

int tdx_gio_plan_rect(struct tdx_gio_state *s, int x, int y, int w, int h, uint32_t comp_len,
		      uint8_t pkts[4][TDX_GIO_PKT])
{
	if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > TDX_GIO_W || y + h > TDX_GIO_H)
		return -1;
	uint16_t mx = (uint16_t)(TDX_GIO_W - w - x);
	tdx_gio_cmd_write_rect(pkts[0], (uint8_t)s->a, comp_len, (uint16_t)y, mx, (uint16_t)h,
			       (uint16_t)w);
	int t = s->b; /* flip A <-> B */
	s->b = s->a;
	s->a = t;
	tdx_gio_cmd_show(pkts[1], (uint8_t)s->b);
	/* bring just that rectangle into the new back buffer, then refresh C */
	tdx_gio_cmd_copy_rect(pkts[2], (uint8_t)s->b, (uint8_t)s->a, (uint16_t)y, mx, (uint16_t)y,
			      mx, (uint16_t)h, (uint16_t)w);
	tdx_gio_cmd_copy(pkts[3], (uint8_t)s->b, (uint8_t)s->c);
	s->page_in_c = -1;
	return 4;
}

int tdx_giolcd_show_rect(struct tdx_giolcd *l, const uint32_t *xrgb, int x, int y, int w, int h)
{
	if (l->fd < 0)
		return -EBADF;
	if (x == 0 && y == 0 && w == TDX_GIO_W && h == TDX_GIO_H)
		return tdx_giolcd_show(l, xrgb); /* Windows uses 0x20 for the full panel */
	if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > TDX_GIO_W || y + h > TDX_GIO_H)
		return -EINVAL;
	tdx_gio_pack_rect(xrgb, x, y, w, h, l->raw, l->swap_rb);
	size_t raw = (size_t)w * (size_t)h * 3;
	long n = tdx_deflate_fixed(l->raw, raw, l->comp, sizeof l->comp);
	if (n < 0 || (uint32_t)n > tdx_gio_comp_max(w, h))
		return -EFBIG;
	uint8_t pkts[4][TDX_GIO_PKT];
	tdx_gio_plan_rect(&l->st, x, y, w, h, (uint32_t)n, pkts);
	int rc;
	if ((rc = send_cmd(l, pkts[0])) ||
	    (rc = tdx_usb_bulk_out(l->fd, l->ep_out, l->comp, (size_t)n, 5000)))
		return rc;
	for (int i = 1; i < 4; i++)
		if ((rc = send_cmd(l, pkts[i])))
			return rc;
	return 0;
}

int tdx_giolcd_sync_back(struct tdx_giolcd *l)
{
	if (l->fd < 0)
		return -EBADF;
	uint8_t pkt[TDX_GIO_PKT];
	tdx_gio_cmd_copy(pkt, (uint8_t)l->st.b, (uint8_t)l->st.a);
	return send_cmd(l, pkt);
}

/* ---- legacy sp4LCD.bmp path (0x13) ---- */

static uint32_t rd_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int tdx_bmp_load(const char *path, struct tdx_bmp *bmp)
{
	memset(bmp, 0, sizeof *bmp);
	FILE *f = fopen(path, "rb");
	if (!f)
		return -errno;
	uint8_t h[54];
	int rc = -EINVAL;
	if (fread(h, 1, sizeof h, f) != sizeof h || h[0] != 'B' || h[1] != 'M')
		goto out;
	uint32_t off = rd_le32(h + 10), hsize = rd_le32(h + 14);
	int32_t w = (int32_t)rd_le32(h + 18), ht = (int32_t)rd_le32(h + 22);
	uint16_t planes = rd_le16(h + 26), bpp = rd_le16(h + 28);
	uint32_t comp = rd_le32(h + 30);
	/* uncompressed (BI_RGB) or BI_BITFIELDS 32 bpp, as a DIB section would hold them */
	if (hsize < 40 || w <= 0 || ht == 0 || w > 8192 || ht > 8192 || ht < -8192 ||
	    !(comp == 0 || (comp == 3 && bpp == 32)))
		goto out;
	bmp->width = w;
	bmp->top_down = ht < 0;
	bmp->height = ht < 0 ? -ht : ht;
	bmp->bpp = bpp;
	bmp->planes = planes;
	bmp->stride = (int)(((uint32_t)w * bpp + 31) / 32 * 4);
	size_t size = (size_t)bmp->stride * (size_t)bmp->height;
	bmp->bits = calloc(1, size ? size : 1);
	if (!bmp->bits) {
		rc = -ENOMEM;
		goto out;
	}
	if (fseek(f, (long)off, SEEK_SET) || fread(bmp->bits, 1, size, f) != size) {
		tdx_bmp_free(bmp);
		goto out;
	}
	rc = 0;
out:
	fclose(f);
	return rc;
}

void tdx_bmp_free(struct tdx_bmp *bmp)
{
	free(bmp->bits);
	bmp->bits = NULL;
}

void tdx_gio_bmp_control(const char *path, int *x, int *y, int *w, int *h)
{
	/* FUN_1800438d0: fwscanf(f, L"(%d, %d), (%d, %d)", &x, &y, &w, &h) */
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f)
		return;
	if (fscanf(f, "(%d, %d), (%d, %d)", x, y, w, h) < 0) {
		/* nothing matched: defaults stay */
	}
	fclose(f);
}

long tdx_gio_bmp_convert(const struct tdx_bmp *bmp, int x, int y, int w, int h, uint8_t *out,
			 size_t cap)
{
	if (bmp->planes * bmp->bpp <= 8 || w <= 0 || h <= 0)
		return -1;
	size_t n = (size_t)(bmp->bpp >> 3) * (size_t)h * (size_t)w;
	if (n > cap)
		return -1;
	memset(out, 0, n);
	int row_end = y + h < bmp->height ? y + h : bmp->height;
	for (int row = y; row < row_end; row++) {
		if (row < 0 || row >= TDX_GIO_H) /* FUN_18004a620: row >= descriptor height */
			continue;
		/* GetObject's bmBits, read as bottom-up: row r at (bmHeight - 1 - r) */
		const uint8_t *src = bmp->bits + (size_t)(bmp->height - 1 - row) * (size_t)bmp->stride;
		int col_end = x + w < TDX_GIO_W ? x + w : TDX_GIO_W;
		for (int col = x; col < col_end; col++) {
			long dst = ((long)(w - 1 - (col - x)) * h + (row - y)) * 3;
			if (dst < 0 || dst + 2 >= (long)n)
				break;
			long s = 3L * col; /* always 3 bytes per source pixel, as on Windows */
			if (s < 0 || s + 2 >= bmp->stride)
				break;      /* Windows would read past the row; tdxd stops instead */
			out[dst] = src[s];
			out[dst + 1] = src[s + 1];
			out[dst + 2] = src[s + 2];
		}
	}
	return (long)n;
}

void tdx_gio_cmd_bmp_header(uint8_t pkt[TDX_GIO_PKT], uint32_t len, uint16_t y, uint16_t mx,
			    uint16_t h, uint16_t w)
{
	memset(pkt, 0, TDX_GIO_PKT);
	pkt[0] = 0x13;
	pkt[1] = 0x0F;
	pkt[2] = (uint8_t)len;
	pkt[3] = (uint8_t)(len >> 8);
	pkt[4] = (uint8_t)(len >> 16);
	pkt[5] = (uint8_t)(len >> 24);
	const uint16_t f[4] = {y, mx, h, w}; /* byte 6 = 0 */
	for (int i = 0; i < 4; i++) {
		pkt[7 + 2 * i] = (uint8_t)f[i];
		pkt[8 + 2 * i] = (uint8_t)(f[i] >> 8);
	}
	pkt[15] = 0x0F;
}

int tdx_giolcd_send_bmp(struct tdx_giolcd *l, const char *bmp_path, const char *control)
{
	if (l->fd < 0)
		return -EBADF;
	int x = 0, y = 0, w = TDX_GIO_W, h = TDX_GIO_H;
	tdx_gio_bmp_control(control, &x, &y, &w, &h);
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
		return -EINVAL;
	struct tdx_bmp bmp;
	int rc = tdx_bmp_load(bmp_path, &bmp);
	if (rc)
		return rc;
	size_t cap = (size_t)4 * (size_t)w * (size_t)h;
	uint8_t *raw = malloc(cap);
	uint8_t *comp = malloc(TDX_DEFLATE_BOUND(cap));
	long n = -1, c = -1;
	if (raw && comp)
		n = tdx_gio_bmp_convert(&bmp, x, y, w, h, raw, cap);
	if (n > 0)
		c = tdx_deflate_fixed(raw, (size_t)n, comp, TDX_DEFLATE_BOUND(cap));
	/* FUN_18004a8a0: output buffer max(n + n/0x7c00*5 + 0x85, n*110/100 + 0x80) */
	uint32_t lim = (uint32_t)n + (uint32_t)n / 0x7c00 * 5 + 0x85;
	uint32_t lim2 = (uint32_t)((uint64_t)n * 0x6e / 100) + 0x80;
	if (lim2 > lim)
		lim = lim2;
	if (!raw || !comp)
		rc = -ENOMEM;
	else if (n <= 0)
		rc = -EINVAL;
	else if (c < 0 || (uint32_t)c > lim)
		rc = -EFBIG;
	else {
		uint8_t pkt[TDX_GIO_PKT];
		tdx_gio_cmd_bmp_header(pkt, (uint32_t)c, (uint16_t)y, (uint16_t)(TDX_GIO_W - w - x),
				       (uint16_t)h, (uint16_t)w);
		rc = send_cmd(l, pkt);
		if (!rc)
			rc = tdx_usb_bulk_out(l->fd, l->ep_out, comp, (size_t)c, 5000);
	}
	free(raw);
	free(comp);
	tdx_bmp_free(&bmp);
	return rc;
}

int tdx_gio_plan_move(struct tdx_gio_state *s, int x, int y, struct tdx_gio_step *steps, int max)
{
	/* FUN_180049fd0, operation type 0 */
	if (y < 0) /* "-y + 0x1a0 < 0x1a1": only negative y is rejected */
		return -1;
	int n = 0, k;
	int vis = TDX_GIO_MOVE_LIMIT - y;
	if (vis > 0) { /* moving image (D rows 60..) drawn at y + 60, clipped to the panel */
		if ((k = add_cmd(steps, &n, max)) < 0) return -1;
		tdx_gio_cmd_copy_rect(steps[k].pkt, (uint8_t)s->d, (uint8_t)s->a, TDX_GIO_SLIDE_Y, 0,
				      (uint16_t)(y + TDX_GIO_SLIDE_Y), 0, (uint16_t)vis, TDX_GIO_W);
	}
	int prev = s->move_y;
	if (prev < y) { /* rows uncovered since last step <- C */
		if (prev > TDX_GIO_MOVE_MAX_Y || y > TDX_GIO_MOVE_MAX_Y)
			return n; /* Windows bails out here without recording the position */
		if ((k = add_cmd(steps, &n, max)) < 0) return -1;
		tdx_gio_cmd_copy_rect(steps[k].pkt, (uint8_t)s->c, (uint8_t)s->a, (uint16_t)prev, 0,
				      (uint16_t)prev, 0, (uint16_t)(y - prev), TDX_GIO_W);
	}
	s->move_x = x;
	s->move_y = y;
	return n;
}

int tdx_giolcd_set_moving(struct tdx_giolcd *l, const uint32_t *moving, const uint32_t *background)
{
	if (l->fd < 0)
		return -EBADF;
	/* FUN_18004b2e0 usage 1/2: moving image (slot 0,1) -> D, slot (2,1) -> C, no flip */
	int rc = upload(l, l->st.d, moving);
	if (!rc)
		rc = upload(l, l->st.c, background);
	l->st.page_in_c = l->st.page_in_d = -1; /* the slide page cache is overwritten */
	l->st.move_x = l->st.move_y = 0;
	return rc;
}

int tdx_giolcd_move(struct tdx_giolcd *l, int y, int present)
{
	if (l->fd < 0)
		return -EBADF;
	struct tdx_gio_step steps[3];
	int n = tdx_gio_plan_move(&l->st, l->st.move_x, y, steps, 2);
	if (n < 0)
		return -EINVAL;
	if (present) { /* tdxd addition: Windows leaves A unshown in this operation */
		steps[n].kind = TDX_GIO_STEP_CMD;
		tdx_gio_cmd_show(steps[n++].pkt, (uint8_t)l->st.a);
	}
	for (int i = 0; i < n; i++) {
		int rc = send_cmd(l, steps[i].pkt);
		if (rc)
			return rc;
	}
	return 0;
}

int tdx_giolcd_move_finish(struct tdx_giolcd *l)
{
	if (l->fd < 0)
		return -EBADF;
	/* A has been shown: make it the front buffer and copy it into the new back */
	int t = l->st.b;
	l->st.b = l->st.a;
	l->st.a = t;
	uint8_t pkt[TDX_GIO_PKT];
	tdx_gio_cmd_copy(pkt, (uint8_t)l->st.b, (uint8_t)l->st.a);
	return send_cmd(l, pkt);
}
