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

/* Number of logical buttons exposed for a device. */
int tdx_button_count(const struct tdx_device_def *dev);

#endif
