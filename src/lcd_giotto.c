#define _GNU_SOURCE
#include "lcd_giotto.h"

#include <errno.h>
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
