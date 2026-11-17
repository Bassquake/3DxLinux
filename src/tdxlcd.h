/*
 * libtdxlcd - LCD applet API for tdx-lcdmgr (Linux).
 *
 * Source-compatible with the function names, structures, constants and
 * semantics of Logitech's lglcd API (LCD SDK 3.06), so applets written for
 * LCDMon can be ported by swapping the include and linking -ltdxlcd. Strings
 * are UTF-8 (char); the W/A variants are both provided and identical.
 * Callbacks run on the library's reader thread, one at a time.
 * Return values are the Win32 error codes lglcd documents (0 = success).
 */
#ifndef TDXLCD_H
#define TDXLCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t DWORD;
typedef int BOOL;
typedef uint8_t BYTE;
typedef void *PVOID;

#define LGLCD_INVALID_CONNECTION (-1)
#define LGLCD_INVALID_DEVICE (-1)

/* soft buttons */
#define LGLCDBUTTON_LEFT    0x00000100
#define LGLCDBUTTON_RIGHT   0x00000200
#define LGLCDBUTTON_OK      0x00000400
#define LGLCDBUTTON_CANCEL  0x00000800
#define LGLCDBUTTON_UP      0x00001000
#define LGLCDBUTTON_DOWN    0x00002000
#define LGLCDBUTTON_MENU    0x00004000
#define LGLCDBUTTON_BUTTON0 0x00000001
#define LGLCDBUTTON_BUTTON1 0x00000002
#define LGLCDBUTTON_BUTTON2 0x00000004
#define LGLCDBUTTON_BUTTON3 0x00000008

/* bitmaps */
#define LGLCD_BMP_FORMAT_160x43x1 0x00000001
#define LGLCD_BMP_FORMAT_QVGAx32  0x00000003
#define LGLCD_BW_BMP_WIDTH   160
#define LGLCD_BW_BMP_HEIGHT  43
#define LGLCD_BW_BMP_BPP     1
#define LGLCD_QVGA_BMP_WIDTH  320
#define LGLCD_QVGA_BMP_HEIGHT 240
#define LGLCD_QVGA_BMP_BPP    4

typedef struct { DWORD Format; } lgLcdBitmapHeader;
typedef struct {
	lgLcdBitmapHeader hdr;
	BYTE pixels[LGLCD_BW_BMP_WIDTH * LGLCD_BW_BMP_HEIGHT * LGLCD_BW_BMP_BPP];
} lgLcdBitmap160x43x1;
typedef struct {
	lgLcdBitmapHeader hdr; /* LGLCD_BMP_FORMAT_QVGAx32: B, G, R, A per pixel, row-major */
	BYTE pixels[LGLCD_QVGA_BMP_WIDTH * LGLCD_QVGA_BMP_HEIGHT * LGLCD_QVGA_BMP_BPP];
} lgLcdBitmapQVGAx32;
typedef union {
	lgLcdBitmapHeader hdr;
	lgLcdBitmap160x43x1 bmp_mono;
	lgLcdBitmapQVGAx32 bmp_qvga32;
} lgLcdBitmap;

/* priorities and update modes */
#define LGLCD_PRIORITY_IDLE_NO_SHOW 0
#define LGLCD_PRIORITY_BACKGROUND   64
#define LGLCD_PRIORITY_NORMAL       128
#define LGLCD_PRIORITY_ALERT        255
#define LGLCD_SYNC_UPDATE(p)               (0x80000000u | (p))
#define LGLCD_SYNC_COMPLETE_WITHIN_FRAME(p) (0xC0000000u | (p))
#define LGLCD_ASYNC_UPDATE(p)              (p)
#define LGLCD_LCD_FOREGROUND_APP_NO  0
#define LGLCD_LCD_FOREGROUND_APP_YES 1

/* device families */
#define LGLCD_DEVICE_FAMILY_QVGA_BASIC  0x00000020 /* SpacePilot PRO (FamilyId 32) */
#define LGLCD_DEVICE_FAMILY_QVGA_GAMING 0x00000040
#define LGLCD_DEVICE_FAMILY_ALL_QVGA    0x00000060
#define LGLCD_DEVICE_FAMILY_ALL         0x000001FF
#define LGLCD_DEVICE_FAMILY_OTHER       0x80000000

/* applet capabilities */
#define LGLCD_APPLET_CAP_BASIC 0x00000000
#define LGLCD_APPLET_CAP_BW    0x00000001
#define LGLCD_APPLET_CAP_QVGA  0x00000002

/* notifications */
#define LGLCD_NOTIFICATION_DEVICE_ARRIVAL   1
#define LGLCD_NOTIFICATION_DEVICE_REMOVAL   2
#define LGLCD_NOTIFICATION_CLOSE_CONNECTION 3
#define LGLCD_NOTIFICATION_APPLET_DISABLED  4
#define LGLCD_NOTIFICATION_APPLET_ENABLED   5
#define LGLCD_NOTIFICATION_TERMINATE_APPLET 6
#define LGLCD_DEVICE_BW   1
#define LGLCD_DEVICE_QVGA 2

typedef DWORD (*lgLcdOnSoftButtonsCB)(int device, DWORD dwButtons, const PVOID pContext);
typedef DWORD (*lgLcdOnConfigureCB)(int connection, const PVOID pContext);
typedef DWORD (*lgLcdOnNotificationCB)(int connection, const PVOID pContext, DWORD code,
				       DWORD p1, DWORD p2, DWORD p3, DWORD p4);

typedef struct { lgLcdOnConfigureCB configCallback; PVOID configContext; } lgLcdConfigureContext;
typedef struct { lgLcdOnNotificationCB notificationCallback; PVOID notifyContext; } lgLcdNotificationContext;
typedef struct { lgLcdOnSoftButtonsCB softbuttonsChangedCallback; PVOID softbuttonsChangedContext; }
	lgLcdSoftbuttonsChangedContext;

typedef struct {
	const char *appFriendlyName;
	BOOL isPersistent;
	BOOL isAutostartable;
	lgLcdConfigureContext onConfigure;
	int connection; /* out */
} lgLcdConnectContext;

typedef struct {
	const char *appFriendlyName;
	BOOL isPersistent;
	BOOL isAutostartable;
	lgLcdConfigureContext onConfigure;
	int connection; /* out */
	DWORD dwAppletCapabilitiesSupported;
	DWORD dwReserved1;
	lgLcdNotificationContext onNotify;
} lgLcdConnectContextEx;

typedef struct { DWORD Width, Height, Bpp, NumSoftButtons; } lgLcdDeviceDesc;
typedef struct {
	DWORD deviceFamilyId;
	char deviceDisplayName[260];
	DWORD Width, Height, Bpp, NumSoftButtons, Reserved1, Reserved2;
} lgLcdDeviceDescEx;

typedef struct {
	int connection;
	int index;
	lgLcdSoftbuttonsChangedContext onSoftbuttonsChanged;
	int device; /* out */
} lgLcdOpenContext;

typedef struct {
	int connection;
	int deviceType; /* LGLCD_DEVICE_BW or LGLCD_DEVICE_QVGA */
	lgLcdSoftbuttonsChangedContext onSoftbuttonsChanged;
	int device; /* out */
} lgLcdOpenByTypeContext;

/* W and A spellings are the same function on Linux */
typedef lgLcdConnectContext lgLcdConnectContextA, lgLcdConnectContextW;
typedef lgLcdConnectContextEx lgLcdConnectContextExA, lgLcdConnectContextExW;
typedef lgLcdDeviceDescEx lgLcdDeviceDescExA, lgLcdDeviceDescExW;

DWORD lgLcdInit(void);
DWORD lgLcdDeInit(void);
DWORD lgLcdConnect(lgLcdConnectContext *ctx);
DWORD lgLcdConnectEx(lgLcdConnectContextEx *ctx);
DWORD lgLcdDisconnect(int connection);
DWORD lgLcdSetDeviceFamiliesToUse(int connection, DWORD dwDeviceFamiliesSupported, DWORD dwReserved1);
DWORD lgLcdEnumerate(int connection, int index, lgLcdDeviceDesc *description);
DWORD lgLcdEnumerateEx(int connection, int index, lgLcdDeviceDescEx *description);
DWORD lgLcdOpen(lgLcdOpenContext *ctx);
DWORD lgLcdOpenByType(lgLcdOpenByTypeContext *ctx);
DWORD lgLcdClose(int device);
DWORD lgLcdReadSoftButtons(int device, DWORD *buttons);
DWORD lgLcdUpdateBitmap(int device, const lgLcdBitmapHeader *bitmap, DWORD priority);
DWORD lgLcdSetAsLCDForegroundApp(int device, int foregroundYesNoFlag);

#define lgLcdConnectA lgLcdConnect
#define lgLcdConnectW lgLcdConnect
#define lgLcdConnectExA lgLcdConnectEx
#define lgLcdConnectExW lgLcdConnectEx
#define lgLcdEnumerateExA lgLcdEnumerateEx
#define lgLcdEnumerateExW lgLcdEnumerateEx

/* Linux extensions */
void tdxLcdSetSocketPath(const char *path); /* NULL = default */
/* The icon shown for this program in the LCD's program list (LCDMon took the
 * .exe icon): width x height (<= 64) RGBA pixels, row-major. */
DWORD tdxLcdSetIcon(int connection, int width, int height, const BYTE *rgba);

#ifdef __cplusplus
}
#endif
#endif
