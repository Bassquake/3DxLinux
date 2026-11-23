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

#include "config.h"
#include "devices.h"

#define TDX_RCV_SLOTS 5
#define TDX_RCV_REPORT_SELF 0x42
#define TDX_RCV_REPORT_SLOT0 0x43
#define TDX_RCV_REPORT_BATTERY 0x17
#define TDX_RCV_REPORT_MOUSE_CONFIG 0x10
#define TDX_RCV_MOUSE_CONFIG_LEN 32

/* Pairing (3DxPair.exe's EuclidControl, checked with a CadMouse Compact
 * Wireless): feature report 0x41 (4 data bytes). 41 02 MODE turns pairing on
 * for a kind of device (3DxPair's PairingMode) or off (0); 41 04 SLOT frees a
 * slot. The new device shows up in the slot table (0x43 + slot); the receiver
 * then drops off USB for about 2 s and comes back, as it does after a slot is
 * freed. 3DxPair gives up after 55 s. */
#define TDX_RCV_REPORT_CONTROL 0x41
#define TDX_RCV_PAIRING_SECONDS 55
enum {
	TDX_RCV_PAIR_OFF = 0,
	TDX_RCV_PAIR_ROUND_ROBIN = 1, /* 3DxPair: SpaceMouse Wireless / Pro Wireless */
	TDX_RCV_PAIR_EUCLID = 2,      /* 3DxPair: a CadMouse ("Euclid") */
};
/* The 5-byte SET_FEATURE (report id first) for pairing mode @mode / freeing @slot. */
void tdx_rcv_cmd_pairing(uint8_t out[5], int mode);
void tdx_rcv_cmd_unpair(uint8_t out[5], int slot);

/* HIDDev's class codes for paired devices */
enum { TDX_RCV_3DMOUSE = 1, TDX_RCV_2DMOUSE = 2, TDX_RCV_RECEIVER = 4, TDX_RCV_KEYBOARD = 5 };

struct tdx_rcv_slot {
	uint16_t code;   /* 0 = empty */
	uint16_t pid;    /* 0 if the code is not in 3DxWare's table */
	int cls;         /* TDX_RCV_* or 0 */
	char id[11];     /* the device id, hex */
	char serial[24]; /* 3DxWare's serial number: the code's letters and the id in
			  * decimal (Y + 0x48f7ba8b = Y1224194699) */
};

/* Device code -> product id / class (HIDDev.dll's table at 0x1800dbca0).
 * Returns 0 for unknown codes. */
uint16_t tdx_rcv_code_pid(uint16_t code, int *cls);

/* Parse a feature reply (report id in buf[0]). Returns 1 for a paired
 * device, 0 for an empty slot, -1 for a short or wrong report. */
int tdx_rcv_parse_slot(const uint8_t *buf, size_t n, struct tdx_rcv_slot *out);

/* The CadMouse configuration, HID feature report 0x10 (31 data bytes) on the
 * mouse's interface: what HIDDev.dll's SBHID_SendPIDConfig_2DMouse builds and
 * Mgl3DCtlrRPCService sends with IOCTL_HID_SET_FEATURE. The mouse applies it
 * itself. Data bytes: [1] DPI / 50; [2] lift threshold (0 = lift detection
 * on); [3..6] wheel mode (01 ff 00 00 normal, 00 00 00 01 Smart Scrolling,
 * 3DxWare's "inertial wheel emulation"); [18..24] the button map for buttons
 * 1..7 (3DxWare's default 10 11 12 12 14 13 47; 3DxWare's "Swap Middle /
 * Right" sends 10 12 11 ...: see tdx_rcv_mouse_map); [26] 0x1e; [30] the
 * update-rate divider (1, 2,
 * 4, 8 = 1000, 500, 250, 125 Hz). The values match 3DxWare's own log for a
 * CadMouse Compact Wireless, and the mouse takes the block (its DPI changes;
 * 2026-11-22). Writes @rep (report id first). */
void tdx_rcv_mouse_config(uint8_t rep[TDX_RCV_MOUSE_CONFIG_LEN], int dpi, int rate_hz, int smart,
			  const uint8_t map[7]);

/* The button map: 9 + n makes a button mouse button n (1 left, 2 right,
 * 3 middle, 4 back, 5 forward), done by the mouse itself; 40 + its own number
 * makes it report in input report 0x03 instead (bit = button - 1), which
 * moves nothing: 3DxWare's "sent only to 3DxService", tdxd's keystrokes.
 * From the bindings of @dev's V2DK buttons in @cfg: key_V2DK_FORWARD =
 * BTN_EXTRA (a mouse function), = KEY_... (keystrokes); unbound, 3DxWare's
 * default (the radial-menu button reports and does nothing on Linux). Left
 * and right are not remapped, as in 3DxWare. Returns the bitmask of buttons
 * that report in 0x03 (for the keystrokes). */
unsigned tdx_rcv_mouse_map(const struct tdx_config *cfg, const struct tdx_device_def *dev,
			   uint8_t map[7]);

/* @dev's driver action for button @idx (action_V2DK_... = speed_up, ...,
 * disabled), by its V2DK name only, or TDX_ACT_NONE. It beats a key binding,
 * as on the 3D mice. */
int tdx_rcv_mouse_action(const struct tdx_config *cfg, const struct tdx_device_def *dev, int idx);

/* @dev's binding for button @idx, by its V2DK name only (not BUTTON_n), or
 * NULL when it has none. */
const struct tdx_keymap *tdx_rcv_mouse_binding(const struct tdx_config *cfg,
					       const struct tdx_device_def *dev, int idx);

/* Parse an input report. Returns 1 if it is the battery report, with the
 * level (-1 when the device does not know) and the charging flag. */
int tdx_rcv_parse_battery(const uint8_t *buf, size_t n, int *level, int *charging);

#endif
