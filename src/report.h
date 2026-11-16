/*
 * HID input report decoding for 3Dconnexion devices.
 *
 * Layout recovered from 3dxhid.sys (KMDF HID filter in 3DxWinCore64.msi),
 * functions FUN_14000359c / FUN_1400037e4 (parse / serialize) and
 * FUN_140002be0 (HidP_GetCaps -> "combined" flag when InputReportByteLength>13):
 *
 *   ID 0x01  int16le tx, ty, tz            [+ rx, ry, rz if report is 13 bytes]
 *   ID 0x02  int16le rx, ry, rz
 *   ID 0x03  button bitmask, little endian, 1..4 bytes
 *   ID 0x1C  up to 6 x uint16le "pressed button numbers" (Enterprise, Giotto)
 *   ID 0x1D  uint32le long-press bitmask (+ padding)
 */
#ifndef TDX_REPORT_H
#define TDX_REPORT_H

#include <stddef.h>
#include <stdint.h>
#include "devices.h"

#define TDX_MAX_BUTTONS 64

struct tdx_state {
	int16_t axis[6];          /* tx ty tz rx ry rz, raw device units */
	uint8_t axis_dirty;       /* bit0: translation, bit1: rotation   */
	uint8_t btn[TDX_MAX_BUTTONS]; /* logical button index -> pressed */
	/* raw bookkeeping */
	uint32_t mask03;
	uint16_t numbers1c[6];
	uint32_t lp1d;
};

/*
 * Feed one raw hidraw report (first byte = report id).
 * Returns bitmask: 1 = axes changed, 2 = buttons changed.
 * If @dev has no button table, bit n of report 0x03 maps to button n.
 */
int tdx_report_feed(struct tdx_state *st, const struct tdx_device_def *dev,
		    const uint8_t *buf, size_t len);

/*
 * Build the LED output report (HIDDev.dll FUN_180033ab0): {0x04, state}.
 * state = 1 on, 0 off. Giotto (C63C) uses a different, unrecovered path.
 * Returns report length (2) or 0 if unsupported for this device.
 */
int tdx_led_report(const struct tdx_device_def *dev, int on, uint8_t out[2]);

/*
 * RGB LEDs (Giotto C63C) - HIDDev.dll FUN_180034af0 ("CUSBDev::SetLEDColor"):
 *   HID feature report, 8 bytes: {0x1A, 0x25, index, R, G, B, 0, 0}
 * index 0x00..0x1D  button LEDs (Base.xml <LEDNumber>), 0x1E..0x23 ring segments,
 *       0x24 used with black to switch every LED off       (FUN_180033cd0)
 *       0x25 sets all button LEDs to one colour            (FUN_180033cd0)
 *       0x26 sets all ring segments to one colour          (FUN_180032e70 case 10)
 */
#define TDX_RGB_REPORT_ID   0x1A
#define TDX_RGB_CMD_COLOR   0x25
#define TDX_RGB_NUM_BUTTONS 30
#define TDX_RGB_RING_FIRST  0x1E
#define TDX_RGB_NUM_RING    6
#define TDX_RGB_ALL_OFF     0x24
#define TDX_RGB_ALL_BUTTONS 0x25
#define TDX_RGB_RING        0x26

void tdx_rgb_report(uint8_t index, uint32_t rgb, uint8_t out[8]);
/* Does this device use the RGB feature-report LED path? */
int tdx_has_rgb_leds(const struct tdx_device_def *dev);

/*
 * SpaceMouse Enterprise (C633) LCD intensity - HID feature report {0x11, percent}.
 * HIDDev.dll FUN_180032e70 case 8 (UdDevSetDisplay) and FUN_18002b9f0 (sent on open,
 * then 3 s settle). The value is 3DxService's <LCDIntensity> (Driver_LCDOn -> 100,
 * Driver_LCDOff -> 0). Returns report length (2) or 0 if not applicable.
 */
#define TDX_LCD_INTENSITY_REPORT_ID 0x11
int tdx_lcd_intensity_report(const struct tdx_device_def *dev, int percent, uint8_t out[2]);

/*
 * Re-zero / calibrate (HIDDev.dll UdDevRezero, the GUI's "Calibrate" button and the
 * Driver_RezeroDevice action): HID feature report {0x07, 0x00}. Windows sends it only
 * when the device's HID VersionNumber (USB bcdDevice) is >= 0x200, and for the
 * SpaceMouse Enterprise (C633) only when it is > 0x441. @version 0 = unknown (sent).
 * Returns report length (2) or 0 when Windows would not send it.
 */
#define TDX_REZERO_REPORT_ID 0x07
int tdx_rezero_report(const struct tdx_device_def *dev, uint16_t version, uint8_t out[2]);

/* Number of logical buttons exposed for a device. */
int tdx_button_count(const struct tdx_device_def *dev);

#endif
