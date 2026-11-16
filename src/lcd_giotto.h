/*
 * Giotto (256f:c63c) 1280x416 colour LCD on its WinUSB (vendor) interface.
 *
 * Recovered from 3DxWare HIDDev.dll (CWinUSBLCDMgr):
 *   DAT_1800de3e0 image descriptor {pid C63C, rotate=1, 1280, 416, 3 bytes/px, 415}
 *   FUN_18004a760 pixel copy, rotate mode:
 *                 out[((w-1-x)*h + y)*3 + k] = src_bytes(x, y)[k], k = 0..2
 *                 (src = 32 bpp, memory order B,G,R,X -> 24 bpp B,G,R)
 *   FUN_18004aee0 SendImage (buffered): raw deflate (same miniz tdefl as the
 *                 Enterprise), then command packets padded to 64 bytes
 *                 (FUN_180048480: C63C -> 0x40), each ending in 55 AA at [22..23]:
 *     0x20 {0x20, 0, buf, 1, len32, 0...}             write full frame to buffer
 *     0x21 {0x21, 0, buf, 1, len32, 0,0,0,0, y, mx, h, w}  write rectangle
 *     0x22 {0x22, b1, b2}                             buffer-to-buffer copy
 *     0x23 {0x23, b1, b2, ..., rect}                  rectangle copy
 *     0x24 {0x24, buf}                                show buffer
 *   FUN_180048fb0 init: buffers A=1, B=0, C=3, D=2; sends 0x24 {B}.
 *   FUN_18004b2e0 SendImageToDevice_RGB888, static image (usage 0):
 *     0x20 {A} + payload, swap A<->B, 0x24 {B}, 0x22 {B, A}, 0x22 {B, C}
 */
#ifndef TDX_LCD_GIOTTO_H
#define TDX_LCD_GIOTTO_H

#include <stddef.h>
#include <stdint.h>

#define TDX_GIO_W 1280
#define TDX_GIO_H 416
#define TDX_GIO_PIXELS (TDX_GIO_W * TDX_GIO_H)
#define TDX_GIO_RAW (TDX_GIO_PIXELS * 3)              /* 1 597 440 bytes */
#define TDX_GIO_COMP_MAX 0x1ad080                     /* Windows' output buffer */
#define TDX_GIO_PKT 64

enum {
	TDX_GIO_OP_WRITE = 0x20,
	TDX_GIO_OP_WRITE_RECT = 0x21,
	TDX_GIO_OP_COPY = 0x22,
	TDX_GIO_OP_COPY_RECT = 0x23,
	TDX_GIO_OP_SHOW = 0x24,
};

struct tdx_giolcd {
	int fd, iface;
	unsigned ep_out;
	int buf_a, buf_b, buf_c, buf_d; /* device frame buffers (back, front, spare...) */
	int swap_rb;
	char path[64];
	uint8_t raw[TDX_GIO_RAW];
	uint8_t comp[TDX_GIO_RAW + TDX_GIO_RAW / 8 + 16];
};

/* Pure helpers (unit-tested). */
void tdx_gio_pack(const uint32_t *xrgb, uint8_t *out, int swap_rb);
void tdx_gio_cmd_write(uint8_t pkt[TDX_GIO_PKT], uint8_t buf, uint32_t len);
void tdx_gio_cmd_copy(uint8_t pkt[TDX_GIO_PKT], uint8_t b1, uint8_t b2);
void tdx_gio_cmd_show(uint8_t pkt[TDX_GIO_PKT], uint8_t buf);
/* Command packets for one static frame; updates the buffer roles.
 * pkts[0] precedes the compressed payload, pkts[1..3] follow it. */
void tdx_gio_plan_frame(int *buf_a, int *buf_b, int buf_c, uint32_t comp_len,
			uint8_t pkts[4][TDX_GIO_PKT]);

int tdx_giolcd_open(struct tdx_giolcd *l, uint16_t vid, uint16_t pid);
void tdx_giolcd_close(struct tdx_giolcd *l);
/* Show a 1280x416 0x00RRGGBB image (top-down rows). -EFBIG if it does not
 * compress into Windows' output buffer size. */
int tdx_giolcd_show(struct tdx_giolcd *l, const uint32_t *xrgb);

#endif
