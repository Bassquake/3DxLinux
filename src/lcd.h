/*
 * SpacePilot Pro 320x240 colour LCD (USB interface 0, vendor class).
 *
 * Recovered from Logitech's LCD stack shipped in LADPInst.msi:
 *   LGPBTDD.inf   binds USB\VID_046D&PID_C629&MI_00 (same driver as the G19 LCD)
 *   EDMBTDD.dll   FUN_1800015a0  builds the 512-byte frame header
 *                 FUN_180001c40  converts 32bpp XRGB -> RGB565, column-major
 *                 FUN_180001980  WriteFileEx(handle, buf, 0x25a00) -> bulk OUT
 *                 SettingsUpdatedByUser: vendor request 0x0A = brightness
 *   LGPBTDD.sys   IOCTL 0x220010/0x220014 -> URB_FUNCTION_VENDOR_INTERFACE
 *                 (setup bytes: bRequest, wValue, wIndex, wLength, data...)
 */
#ifndef TDX_LCD_H_INCLUDED
#define TDX_LCD_H_INCLUDED

#include <stdint.h>

#define TDX_LCD_W 320
#define TDX_LCD_H 240
#define TDX_LCD_HDR 512
#define TDX_LCD_FRAME (TDX_LCD_HDR + TDX_LCD_W * TDX_LCD_H * 2) /* 0x25a00 */

struct tdx_lcd {
	int fd;           /* usbfs fd, -1 if closed */
	int iface;        /* vendor interface (0)   */
	unsigned ep_out;  /* bulk OUT endpoint      */
	char path[64];
	uint8_t frame[TDX_LCD_FRAME];
};

/* Pure helpers (no I/O), unit-tested. */
void tdx_lcd_header(uint8_t hdr[TDX_LCD_HDR]);
/* xrgb: row-major 320x240, 0x00RRGGBB. out: 153600 bytes. */
void tdx_lcd_pack(const uint32_t *xrgb, uint8_t *out);

/* Device I/O via Linux usbfs (/dev/bus/usb). Return 0 on success, -errno on error. */
int tdx_lcd_open(struct tdx_lcd *lcd, uint16_t vid, uint16_t pid);
void tdx_lcd_close(struct tdx_lcd *lcd);
int tdx_lcd_show(struct tdx_lcd *lcd, const uint32_t *xrgb);
int tdx_lcd_set_brightness(struct tdx_lcd *lcd, int percent); /* 0..100 */
int tdx_lcd_get_brightness(struct tdx_lcd *lcd);              /* >=0 or -errno */

#endif
