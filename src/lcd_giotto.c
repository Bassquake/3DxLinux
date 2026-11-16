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
	l->buf_a = 1; /* FUN_180048fb0 */
	l->buf_b = 0;
	l->buf_c = 3;
	l->buf_d = 2;
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
	tdx_gio_cmd_show(pkt, (uint8_t)l->buf_b); /* init: show the front buffer */
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

int tdx_giolcd_show(struct tdx_giolcd *l, const uint32_t *xrgb)
{
	if (l->fd < 0)
		return -EBADF;
	tdx_gio_pack(xrgb, l->raw, l->swap_rb);
	long n = tdx_deflate_fixed(l->raw, sizeof l->raw, l->comp, sizeof l->comp);
	if (n < 0 || n > TDX_GIO_COMP_MAX)
		return -EFBIG;

	uint8_t pkts[4][TDX_GIO_PKT];
	int rc;
	tdx_gio_plan_frame(&l->buf_a, &l->buf_b, l->buf_c, (uint32_t)n, pkts);
	if ((rc = send_cmd(l, pkts[0])) ||
	    (rc = tdx_usb_bulk_out(l->fd, l->ep_out, l->comp, (size_t)n, 5000)))
		return rc;
	for (int i = 1; i < 4; i++)
		if ((rc = send_cmd(l, pkts[i])))
			return rc;
	return 0;
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
