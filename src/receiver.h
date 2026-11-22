/* 3Dconnexion wireless receivers (Universal Receiver 256f:c652 and family).
 *
 * From 3DxWare 10.9.14 HIDDev.dll and the receiver itself:
 *  - every USB interface of the receiver has a vendor collection with feature
 *    reports 0x41..0x47, 0x50, 0x60 and 0x1a (7 data bytes each);
 *  - feature 0x43 + slot (slot 0..4) is the pairing table: data byte 0..1 a
 *    LE device code ('Y' = CadMouse Compact Wireless, 0 = empty slot), the
 *    remaining five bytes the paired device's id;
 *  - feature 0x42 on an interface is the device that interface carries
 *    (interface N = slot N);
 *  - input report 0x17 is the battery: data byte 0 the level in percent
 *    (0 = unknown), byte 1 bit 0 = charging. A connected device sends it
 *    every 1.5 s or so (HIDDev's "Battery Level Packet").
 * Pure parsing only; tdxd does the I/O. */
#ifndef TDX_RECEIVER_H
#define TDX_RECEIVER_H

#include <stddef.h>
#include <stdint.h>

#define TDX_RCV_SLOTS 5
#define TDX_RCV_REPORT_SELF 0x42
#define TDX_RCV_REPORT_SLOT0 0x43
#define TDX_RCV_REPORT_BATTERY 0x17

/* HIDDev's class codes for paired devices */
enum { TDX_RCV_3DMOUSE = 1, TDX_RCV_2DMOUSE = 2, TDX_RCV_RECEIVER = 4, TDX_RCV_KEYBOARD = 5 };

struct tdx_rcv_slot {
	uint16_t code;   /* 0 = empty */
	uint16_t pid;    /* 0 if the code is not in 3DxWare's table */
	int cls;         /* TDX_RCV_* or 0 */
	char id[11];     /* the device id, hex */
};

/* Device code -> product id / class (HIDDev.dll's table at 0x1800dbca0).
 * Returns 0 for unknown codes. */
uint16_t tdx_rcv_code_pid(uint16_t code, int *cls);

/* Parse a feature reply (report id in buf[0]). Returns 1 for a paired
 * device, 0 for an empty slot, -1 for a short or wrong report. */
int tdx_rcv_parse_slot(const uint8_t *buf, size_t n, struct tdx_rcv_slot *out);

/* Parse an input report. Returns 1 if it is the battery report, with the
 * level (-1 when the device does not know) and the charging flag. */
int tdx_rcv_parse_battery(const uint8_t *buf, size_t n, int *level, int *charging);

#endif
