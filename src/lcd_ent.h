/*
 * SpaceMouse Enterprise (256f:c633) 640x150 colour LCD on the WinUSB interface 0.
 *
 * Recovered from 3DxWare HIDDev.dll (CWinUSBLCDMgr):
 *   FUN_18004c440  SendTIFFFileToDevice: 640x150x24bpp strips (0x46500 bytes)
 *   FUN_18004bbd0  SendImage:
 *                  - 24bpp -> RGB565 row-major, 0x0000 replaced by 0x0821
 *                  - delta ("mask") frames: unchanged pixels set to 0
 *                  - raw deflate (miniz tdefl, static Huffman, no zlib header)
 *                  - one header packet (wMaxPacketSize = 512 bytes, zero padded):
 *                      [0] opcode 0x11 full | 0x18 delta | 0x15 first frame after open
 *                      [1] 0x0F
 *                      [2..5] compressed length, u32 LE
 *                      [6..13] 0 0 0 0, 00 3F 01 EF (0x18: all zero)
 *                      [14..15] 00 0F
 *                  - then the compressed bytes on the same bulk OUT pipe
 *                  - after the first frame (0x15): 5 s "waiting for LCD to init"
 *   FUN_180048480  packet size: C633 -> 0x200, C63C -> 0x40
 * Brightness is the HID feature report {0x11, percent} (see report.h).
 */
#ifndef TDX_LCD_ENT_H
#define TDX_LCD_ENT_H

#include <stddef.h>
#include <stdint.h>

#define TDX_ENT_W 640
#define TDX_ENT_H 150
#define TDX_ENT_PIXELS (TDX_ENT_W * TDX_ENT_H)
#define TDX_ENT_RAW (TDX_ENT_PIXELS * 2) /* 0x2ee00 */
#define TDX_ENT_PKT 512
#define TDX_ENT_COMP_MAX 0x33980 /* Windows' tdefl output buffer (avail_out) */

enum { TDX_ENT_OP_FULL = 0x11, TDX_ENT_OP_FIRST = 0x15, TDX_ENT_OP_DELTA = 0x18 };

struct tdx_entlcd {
	int fd, iface;
	unsigned ep_out;
	int need_init;   /* next frame is the first after open (opcode 0x15 + 5 s wait) */
	long long busy_until_ms; /* CLOCK_MONOTONIC; frames before this return -EBUSY */
	int have_prev;
	int swap_rb;     /* treat input as BGR (see docs: channel order inferred) */
	char path[64];
	uint16_t prev[TDX_ENT_PIXELS];
	uint16_t cur[TDX_ENT_PIXELS];
	uint8_t comp[TDX_ENT_RAW + TDX_ENT_RAW / 8 + 16];
};

/* Pure helpers (unit-tested). */
void tdx_ent_pack(const uint32_t *xrgb, uint16_t *out, int swap_rb);
/* Zero pixels equal to @prev (delta frame). Returns number of changed pixels. */
size_t tdx_ent_delta(uint16_t *cur, const uint16_t *prev);
void tdx_ent_header(uint8_t pkt[TDX_ENT_PKT], uint8_t op, uint32_t comp_len);
/* Build a complete transfer (header packet + compressed payload) into caller buffers.
 * Returns compressed payload length or -1. */
long tdx_ent_encode(const uint16_t *pix565, uint8_t op, uint8_t pkt[TDX_ENT_PKT],
		    uint8_t *comp, size_t cap);

int tdx_entlcd_open(struct tdx_entlcd *l, uint16_t vid, uint16_t pid);
void tdx_entlcd_close(struct tdx_entlcd *l);
/* Show a 640x150 0x00RRGGBB image. @allow_delta: send only changed pixels when possible.
 * Returns -EBUSY during the 5 s init wait after the first frame (retry later). */
int tdx_entlcd_show(struct tdx_entlcd *l, const uint32_t *xrgb, int allow_delta);

#endif
