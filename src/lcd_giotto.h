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

/*
 * Slide (FUN_180049fd0, LCDImageOp type 1 "SiImageOpType_Slide"):
 *   up to 4 pages of 1280x416; position x in [0, pages*1280).
 *   page p = x / 1280, off = x % 1280. Device buffer D holds page p, C page p+1
 *   (each written with 0x20 only when it changes). Per step, rows 60..400 of the
 *   back buffer A are composed with two 0x23 rectangle copies, then A/B swap
 *   and 0x24 shows it (Windows then sleeps 25 ms):
 *     0x23 {src D, dst A, sy 60, smx 0,          dy 60, dmx off, h 341, w 1280-off}
 *     0x23 {src C, dst A, sy 60, smx 1280-off,   dy 60, dmx 0,   h 341, w off}
 *   On the last page the page is copied from C at full width.
 *   0x23 layout: [1] src, [2] dst, [8] src y, [10] src mx, [12] dst y,
 *   [14] dst mx, [16] h, [18] w (u16 LE; mx = mirrored x = 1280 - w - x).
 */
#define TDX_GIO_MAX_PAGES 4
#define TDX_GIO_SLIDE_Y 0x3C  /* 60  */
#define TDX_GIO_SLIDE_H 0x155 /* 341 */
#define TDX_GIO_SLIDE_STEP_MS 25

/*
 * Vertical move (FUN_180049fd0, LCDImageOp type 0, "move _Moving to x, y"):
 *   the "moving" image (usage 2, slot (0,1)) sits in buffer D, the background
 *   (slot (2,1)) in C. For y in [0, 416] with previous y = prev (initially 0):
 *     if y < 356:   0x23 {src D, dst A, sy 60,   smx 0, dy y+60, dmx 0, h 356-y,  w 1280}
 *     if y > prev:  0x23 {src C, dst A, sy prev, smx 0, dy prev, dmx 0, h y-prev, w 1280}
 *   then prev = y. Windows sends no 0x24 here; tdxd can optionally present A.
 */
#define TDX_GIO_MOVE_LIMIT 0x164 /* 356 */
#define TDX_GIO_MOVE_MAX_Y 0x1A0 /* 416 */

struct tdx_gio_state {
	int a, b, c, d;              /* device frame buffers: back, front, spare, page */
	int page_in_c, page_in_d;    /* which slide page each holds, -1 = unknown */
	int move_x, move_y;          /* last vertical-move position (+0x2f104/+0x2f108) */
};

enum { TDX_GIO_STEP_UPLOAD = 1, TDX_GIO_STEP_CMD = 2 };
struct tdx_gio_step {
	int kind;
	int buf, page;               /* UPLOAD: write slide page @page into buffer @buf */
	uint8_t pkt[TDX_GIO_PKT];    /* CMD */
};

struct tdx_giolcd {
	int fd, iface;
	unsigned ep_out;
	struct tdx_gio_state st;        /* device buffer roles + slide page cache */
	const uint32_t *pages[TDX_GIO_MAX_PAGES];
	int npages;
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

void tdx_gio_cmd_copy_rect(uint8_t pkt[TDX_GIO_PKT], uint8_t src, uint8_t dst, uint16_t sy,
			   uint16_t smx, uint16_t dy, uint16_t dmx, uint16_t h, uint16_t w);
/* Plan one slide step at position @x over @npages pages. Updates @s.
 * Returns the number of steps written (<= @max), or -1 on bad arguments. */
int tdx_gio_plan_slide(struct tdx_gio_state *s, int x, int npages,
		       struct tdx_gio_step *steps, int max);

/* Plan one vertical-move step to (@x, @y), exactly as Windows (no 0x24).
 * Returns the number of CMD steps (0..2), or -1 if @y < 0. For y > 416 the
 * restore is skipped and the position is not recorded, as on Windows. */
int tdx_gio_plan_move(struct tdx_gio_state *s, int x, int y, struct tdx_gio_step *steps, int max);

int tdx_giolcd_open(struct tdx_giolcd *l, uint16_t vid, uint16_t pid);
void tdx_giolcd_close(struct tdx_giolcd *l);
/* Show a 1280x416 0x00RRGGBB image (top-down rows). -EFBIG if it does not
 * compress into Windows' output buffer size. */
int tdx_giolcd_show(struct tdx_giolcd *l, const uint32_t *xrgb);
/* Register 1..4 slide pages (1280x416 0x00RRGGBB each; caller keeps them alive). */
int tdx_giolcd_set_pages(struct tdx_giolcd *l, const uint32_t *const *pages, int n);
/* Show slide position @x (0 .. (npages-1)*1280). Call tdx_giolcd_show() once
 * beforehand so the rows outside the slide band are populated. */
int tdx_giolcd_slide(struct tdx_giolcd *l, int x);
/* Upload the vertical-move images: @moving -> D, @background -> C; resets y to 0. */
int tdx_giolcd_set_moving(struct tdx_giolcd *l, const uint32_t *moving, const uint32_t *background);
/* Move the moving image to @y (0..416). With @present, also show buffer A
 * (0x24 {A}; a tdxd addition - Windows does not show it here). */
int tdx_giolcd_move(struct tdx_giolcd *l, int y, int present);
/* End a presented move: make the shown buffer the front (B) and sync the back. */
int tdx_giolcd_move_finish(struct tdx_giolcd *l);

#endif
