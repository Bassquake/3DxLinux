/*
 * SpacePilot Pro 320x240 colour LCD (USB interface 0, vendor class).
 *
 * Recovered from Logitech's LCD stack shipped in LADPInst.msi:
 *   LGPBTDD.inf   binds USB\VID_046D&PID_C629&MI_00 (same driver as the G19 LCD)
 *   EDMBTDD.dll   FUN_1800015a0  builds the 512-byte frame header
 *                 FUN_180001c40  converts 32bpp XRGB -> RGB565, column-major
 *                 FUN_180001980  WriteFileEx(handle, buf, 0x25a00) -> bulk OUT
 *                 SettingsUpdatedByUser: vendor request 0x0A = brightness
 *                 FUN_180002530 (resume): vendor IN request 0x01 = ping
 *   LGPBTDD.sys   IOCTL 0x220010/0x220014 -> URB_FUNCTION_VENDOR_INTERFACE
 *                 (setup bytes: bRequest, wValue, wIndex, wLength, data...)
 */
#ifndef TDX_LCD_H_INCLUDED
#define TDX_LCD_H_INCLUDED

#include <stdint.h>
#include <linux/usbdevice_fs.h>

#define TDX_LCD_W 320
#define TDX_LCD_H 240
#define TDX_LCD_HDR 512
#define TDX_LCD_FRAME (TDX_LCD_HDR + TDX_LCD_W * TDX_LCD_H * 2) /* 0x25a00 */

struct tdx_lcd {
	int fd;           /* usbfs fd, -1 if closed */
	int iface;        /* vendor interface (0)   */
	unsigned ep_out;  /* bulk OUT endpoint      */
	unsigned ep_in;   /* interrupt IN endpoint (soft buttons), 0 if none */
	char path[64];
	struct usbdevfs_urb in_urb; /* pending soft-button read */
	int in_pending;
	uint8_t in_buf[8];
	uint8_t frame[TDX_LCD_FRAME];
};

/*
 * Soft buttons (LCD navigation keys), interface 0 interrupt IN endpoint 0x81,
 * 2-byte reports. EDMBTDD.dll FUN_180001af0 maps byte 0 to lglcd bits:
 *   bit7 UP 0x1000, bit6 DOWN 0x2000, bit5 LEFT 0x100, bit4 RIGHT 0x200,
 *   bit3 OK 0x400, bit2 MENU 0x4000, bit1 CANCEL 0x800, bit0 0x80000000.
 */
#define TDX_SB_LEFT   0x00000100u
#define TDX_SB_RIGHT  0x00000200u
#define TDX_SB_OK     0x00000400u
#define TDX_SB_CANCEL 0x00000800u
#define TDX_SB_UP     0x00001000u
#define TDX_SB_DOWN   0x00002000u
#define TDX_SB_MENU   0x00004000u
#define TDX_SB_SPECIAL 0x80000000u /* bit 0: not an lglcd button; LCDMon-internal */
uint32_t tdx_lcd_softbuttons(uint8_t report0);

/* Pure helpers (no I/O), unit-tested. */
void tdx_lcd_header(uint8_t hdr[TDX_LCD_HDR]);
/* xrgb: row-major 320x240, 0x00RRGGBB. out: 153600 bytes. */
void tdx_lcd_pack(const uint32_t *xrgb, uint8_t *out);

/* Device I/O via Linux usbfs (/dev/bus/usb). Return 0 on success, -errno on error. */
int tdx_lcd_open(struct tdx_lcd *lcd, uint16_t vid, uint16_t pid);
void tdx_lcd_close(struct tdx_lcd *lcd);
int tdx_lcd_show(struct tdx_lcd *lcd, const uint32_t *xrgb);
/* Vendor IN request 0x01 (9 bytes): presence / ready check. 0 = device answered. */
int tdx_lcd_ping(struct tdx_lcd *lcd);
/* Ping until the device answers (max @tries, 100 ms apart), then settle 500 ms,
 * as EDMBTDD.dll does after resume. 0 on success. */
int tdx_lcd_wait_ready(struct tdx_lcd *lcd, int tries);
int tdx_lcd_set_brightness(struct tdx_lcd *lcd, int percent); /* 0..100 */
int tdx_lcd_get_brightness(struct tdx_lcd *lcd);              /* >=0 or -errno */
/* Soft buttons: submit an async interrupt read (0 or -errno). When the usbfs fd
 * polls writable, reap: returns 1 and sets *buttons on a report, 0 if nothing
 * completed, -errno on error (-ENODEV unplugged). Reaping resubmits. */
int tdx_lcd_buttons_start(struct tdx_lcd *lcd);
int tdx_lcd_buttons_reap(struct tdx_lcd *lcd, uint32_t *buttons);

#endif
