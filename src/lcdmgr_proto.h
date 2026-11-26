/*
 * 3dxlinux-lcdmgr applet protocol (Linux replacement for LCDMon's LGLCDPIPE).
 *
 * On Windows, applets link lglcd.lib -> LgLcdApi.dll, which talks to LCDMon.exe
 * over the named pipe \\.\pipe\LGLCDPIPE... On Linux, applets use lib3dxlinuxlcd
 * (same function names as lglcd) or the Python module lcd_3dxlinux, which speak this
 * protocol to 3dxlinux-lcdmgr over a Unix stream socket:
 *
 *     $XDG_RUNTIME_DIR/3dxlinux-lcd.sock   (else /tmp/3dxlinux-lcd-<uid>.sock)
 *
 * One socket = one lglcd "connection". Every message is a header followed by
 * len payload bytes, all integers little-endian. Requests are answered in order
 * by one LM_REPLY; events (LM_EV_*) may arrive at any time between replies.
 */
#ifndef TDX_LCDMGR_PROTO_H
#define TDX_LCDMGR_PROTO_H

#include <stdint.h>

#define LM_VERSION 1
#define LM_MAX_PAYLOAD (16 + 320 * 240 * 4)
#define LM_NAME_MAX 128

struct lm_hdr {
	uint32_t type;
	uint32_t len;
};

enum lm_type {
	/* client -> server requests */
	LM_CONNECT = 1,     /* u32 version, u32 caps, u32 flags (LM_CF_*), char name[] (UTF-8)
			     * -> reply u32 connection id */
	LM_DISCONNECT = 2,  /* -> reply */
	LM_ENUMERATE = 3,   /* u32 index -> reply struct lm_devdesc */
	LM_OPEN = 4,        /* u32 by (0 = index, 1 = type), u32 value -> reply u32 device */
	LM_CLOSE = 5,       /* u32 device */
	LM_UPDATE = 6,      /* u32 device, u32 priority (with LGLCD_SYNC_* bits), u32 format,
			     * pixels: 320*240 BGRA (QVGAx32) or 160*43 bytes (mono) */
	LM_FOREGROUND = 7,  /* u32 device, u32 yes */
	LM_READBUTTONS = 8, /* u32 device -> reply u32 buttons */
	LM_FAMILIES = 9,    /* u32 families (lgLcdSetDeviceFamiliesToUse) */
	LM_ICON = 10,       /* u32 w, u32 h (<= 64), w*h RGBA: the program's icon in the
			     * on-LCD program list (LCDMon extracted it from the .exe) */
	/* server -> client events */
	LM_EV_BUTTONS = 64,   /* u32 device, u32 buttons */
	LM_EV_CONFIGURE = 65, /* (user pressed Configure... in the LCD Manager) */
	LM_EV_NOTIFY = 66,    /* u32 code, u32 p1, p2, p3, p4 */
	LM_REPLY = 128,       /* u32 request type, u32 status (Win32 error code), data */
	/* administration (3dxlinux-settings LCD Manager): UTF-8 text in, text reply */
	LM_ADMIN = 200,
};

/* LM_CONNECT flags (lgLcdConnectContextEx fields) */
#define LM_CF_PERSISTENT   0x01 /* isPersistent: kept in the Programs list */
#define LM_CF_AUTOSTART    0x02 /* isAutostartable: the manager may start it */
#define LM_CF_CONFIGURABLE 0x04 /* has an onConfigure callback */
#define LM_CF_NOTIFY       0x08 /* has an onNotify callback */

struct lm_devdesc {
	uint32_t family, width, height, bpp, softbuttons;
	char name[64];
};

/* lglcd constants the server needs (same values as 3dxlinuxlcd.h) */
#define LGLCD_DEVICE_BW_ 1
#define LGLCD_DEVICE_QVGA_ 2
#define LGLCD_BMP_FORMAT_160x43x1_ 1
#define LGLCD_BMP_FORMAT_QVGAx32_ 3
#define LGLCD_APPLET_CAP_QVGA_ 2
#define LGLCD_NOTIFICATION_DEVICE_ARRIVAL_ 1
#define LGLCD_NOTIFICATION_DEVICE_REMOVAL_ 2
#define LGLCD_NOTIFICATION_CLOSE_CONNECTION_ 3
#define LGLCD_NOTIFICATION_APPLET_DISABLED_ 4
#define LGLCD_NOTIFICATION_APPLET_ENABLED_ 5

/* Status codes: the Win32 values lglcd documents. */
#define LM_OK 0
#define LM_ERROR_FILE_NOT_FOUND 2       /* manager not running */
#define LM_ERROR_ACCESS_DENIED 5
#define LM_ERROR_INVALID_PARAMETER 87
#define LM_ERROR_ALREADY_EXISTS 183
#define LM_ERROR_NO_MORE_ITEMS 259
#define LM_ERROR_SERVICE_NOT_ACTIVE 1062
#define LM_ERROR_OLD_WIN_VERSION 1150   /* protocol version mismatch */
#define LM_ERROR_DEVICE_NOT_CONNECTED 1167
#define LM_ERROR_ALREADY_INITIALIZED 1247
#define LM_ERROR_LOCK_FAILED 167        /* applet disabled by the user */
#define LM_ERROR_NOT_FOUND 1168

#endif
