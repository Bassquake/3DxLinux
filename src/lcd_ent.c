#define _GNU_SOURCE
#include "lcd_ent.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "deflate.h"
#include "usbfs.h"

static long long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define ZERO_SUBST 0x0821 /* FUN_18004bbd0: black is sent as 0x0821, 0 means "unchanged" */

void tdx_ent_pack(const uint32_t *xrgb, uint16_t *out, int swap_rb)
{
	for (size_t i = 0; i < TDX_ENT_PIXELS; i++) {
		uint32_t p = xrgb[i];
		uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
		if (swap_rb) {
			uint32_t t = r;
			r = b;
			b = t;
		}
		uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
		out[i] = v ? v : ZERO_SUBST;
	}
}

size_t tdx_ent_delta(uint16_t *cur, const uint16_t *prev)
{
	size_t changed = 0;
	for (size_t i = 0; i < TDX_ENT_PIXELS; i++) {
		if (cur[i] == prev[i])
			cur[i] = 0;
		else
			changed++;
	}
	return changed;
}

void tdx_ent_header(uint8_t pkt[TDX_ENT_PKT], uint8_t op, uint32_t len)
{
	memset(pkt, 0, TDX_ENT_PKT);
	pkt[0] = op;
	pkt[1] = 0x0F;
	pkt[2] = (uint8_t)len;
	pkt[3] = (uint8_t)(len >> 8);
	pkt[4] = (uint8_t)(len >> 16);
	pkt[5] = (uint8_t)(len >> 24);
	if (op != TDX_ENT_OP_DELTA) { /* 0x18 leaves bytes 6..13 zero */
		pkt[11] = 0x3F; /* bytes 10..13 = 00 3F 01 EF */
		pkt[12] = 0x01;
		pkt[13] = 0xEF;
	}
	pkt[15] = 0x0F; /* bytes 14..15 = 00 0F */
}

long tdx_ent_encode(const uint16_t *pix, uint8_t op, uint8_t pkt[TDX_ENT_PKT],
		    uint8_t *comp, size_t cap)
{
	static uint8_t raw[TDX_ENT_RAW];
	for (size_t i = 0; i < TDX_ENT_PIXELS; i++) { /* little-endian u16, as in memory on x86 */
		raw[2 * i] = (uint8_t)pix[i];
		raw[2 * i + 1] = (uint8_t)(pix[i] >> 8);
	}
	long n = tdx_deflate_fixed(raw, sizeof raw, comp, cap);
	if (n < 0)
		return -1;
	tdx_ent_header(pkt, op, (uint32_t)n);
	return n;
}

int tdx_entlcd_open(struct tdx_entlcd *l, uint16_t vid, uint16_t pid)
{
	l->fd = -1;
	l->iface = 0;
	l->need_init = 1;
	l->have_prev = 0;
	l->busy_until_ms = 0;
	int rc = tdx_usb_find_node(vid, pid, l->path, sizeof l->path);
	if (rc)
		return rc;
	int fd = tdx_usb_claim(l->path, l->iface);
	if (fd < 0)
		return fd;
	l->fd = fd;
	if (tdx_usb_find_bulk_out(fd, l->iface, &l->ep_out))
		l->ep_out = 0x01;
	return 0;
}

void tdx_entlcd_close(struct tdx_entlcd *l)
{
	tdx_usb_release(l->fd, l->iface);
	l->fd = -1;
}

int tdx_entlcd_show(struct tdx_entlcd *l, const uint32_t *xrgb, int allow_delta)
{
	if (l->fd < 0)
		return -EBADF;
	if (l->busy_until_ms && mono_ms() < l->busy_until_ms)
		return -EBUSY;
	l->busy_until_ms = 0;
	uint8_t pkt[TDX_ENT_PKT];
	tdx_ent_pack(xrgb, l->cur, l->swap_rb);

	uint8_t op = TDX_ENT_OP_FULL;
	static uint16_t send[TDX_ENT_PIXELS];
	memcpy(send, l->cur, sizeof send);
	if (l->need_init) {
		op = TDX_ENT_OP_FIRST;
	} else if (allow_delta && l->have_prev) {
		if (tdx_ent_delta(send, l->prev) == 0)
			return 0; /* nothing changed */
		op = TDX_ENT_OP_DELTA;
	}
	long n = tdx_ent_encode(send, op, pkt, l->comp, sizeof l->comp);
	if (n < 0)
		return -EOVERFLOW;
	int rc = tdx_usb_bulk_out(l->fd, l->ep_out, pkt, TDX_ENT_PKT, 1000);
	if (!rc)
		rc = tdx_usb_bulk_out(l->fd, l->ep_out, l->comp, (size_t)n, 2000);
	if (rc)
		return rc;
	memcpy(l->prev, l->cur, sizeof l->prev);
	l->have_prev = 1;
	if (op == TDX_ENT_OP_FIRST) {
		l->need_init = 0;
		/* "SendImage: waiting for LCD to init" - Windows blocks in Sleep(5000) */
		l->busy_until_ms = mono_ms() + 5000;
	}
	return 0;
}
