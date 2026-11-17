/*
 * tdx-lcd-demo - a minimal LCD applet for tdx-lcdmgr, written against the
 * lglcd-compatible API (tdxlcd.h). Shows a counter, the soft buttons pressed
 * and whether "Configure..." was clicked in the LCD Manager.
 *
 *   tdx-lcd-demo [-n NAME] [-p PRIORITY] [-a] [-f] [-s SECONDS]
 *     -a  persistent + autostartable (appears in the Programs list)
 *     -f  ask to be the foreground program
 *     -s  run for SECONDS then exit (default: until Ctrl+C)
 */
#include "gfx.h"
#include "tdxlcd.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t quit;
static volatile DWORD last_buttons;
static volatile int configured, notes;

static void on_sig(int s) { (void)s; quit = 1; }

static DWORD on_buttons(int device, DWORD b, const PVOID ctx)
{
	(void)device, (void)ctx;
	last_buttons = b;
	return 0;
}

static DWORD on_configure(int connection, const PVOID ctx)
{
	(void)connection, (void)ctx;
	configured++;
	return 0;
}

static DWORD on_notify(int connection, const PVOID ctx, DWORD code, DWORD p1, DWORD p2, DWORD p3,
		       DWORD p4)
{
	(void)connection, (void)ctx, (void)p1, (void)p2, (void)p3, (void)p4;
	notes++;
	fprintf(stderr, "tdx-lcd-demo: notification %u\n", (unsigned)code);
	if (code == LGLCD_NOTIFICATION_CLOSE_CONNECTION)
		quit = 1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *name = "LCD Demo";
	int prio = LGLCD_PRIORITY_NORMAL, persist = 0, fg = 0, secs = 0, opt;
	while ((opt = getopt(argc, argv, "n:p:afs:")) != -1) {
		switch (opt) {
		case 'n': name = optarg; break;
		case 'p': prio = atoi(optarg); break;
		case 'a': persist = 1; break;
		case 'f': fg = 1; break;
		case 's': secs = atoi(optarg); break;
		default:
			fputs("usage: tdx-lcd-demo [-n NAME] [-p PRIORITY] [-a] [-f] [-s SECONDS]\n", stderr);
			return 2;
		}
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	lgLcdInit();
	lgLcdConnectContextEx cc;
	memset(&cc, 0, sizeof cc);
	cc.appFriendlyName = name;
	cc.isPersistent = persist;
	cc.isAutostartable = persist;
	cc.onConfigure.configCallback = on_configure;
	cc.dwAppletCapabilitiesSupported = LGLCD_APPLET_CAP_QVGA;
	cc.onNotify.notificationCallback = on_notify;
	DWORD st = lgLcdConnectEx(&cc);
	if (st) {
		fprintf(stderr, "tdx-lcd-demo: connect failed (%u)%s\n", (unsigned)st,
			st == 2 ? ": is tdx-lcdmgr running?" : "");
		return 1;
	}
	lgLcdOpenByTypeContext oc;
	memset(&oc, 0, sizeof oc);
	oc.connection = cc.connection;
	oc.deviceType = LGLCD_DEVICE_QVGA;
	oc.onSoftbuttonsChanged.softbuttonsChangedCallback = on_buttons;
	while ((st = lgLcdOpenByType(&oc)) == 1167 && !quit) /* device not connected yet */
		sleep(1);
	if (st) {
		fprintf(stderr, "tdx-lcd-demo: open failed (%u)\n", (unsigned)st);
		lgLcdDisconnect(cc.connection);
		return 1;
	}
	if (fg)
		lgLcdSetAsLCDForegroundApp(oc.device, LGLCD_LCD_FOREGROUND_APP_YES);
	static uint32_t px[320 * 240];
	static lgLcdBitmapQVGAx32 bmp;
	struct tdx_fb fb = {320, 240, px};
	time_t start = time(NULL);
	for (int n = 0; !quit && (!secs || time(NULL) - start < secs); n++) {
		char line[64];
		gfx_fill(&fb, 0, 0, 320, 240, 0x203040);
		gfx_fill(&fb, 0, 0, 320, 28, 0xC06020);
		gfx_text(&fb, 8, 7, name, 2, 0xFFFFFF);
		snprintf(line, sizeof line, "FRAME %d", n);
		gfx_text(&fb, 12, 50, line, 3, 0xFFFFFF);
		snprintf(line, sizeof line, "BUTTONS %04X", (unsigned)last_buttons);
		gfx_text(&fb, 12, 90, line, 3, 0xFFD040);
		snprintf(line, sizeof line, "CONFIGURE %d", configured);
		gfx_text(&fb, 12, 130, line, 3, 0x80E080);
		snprintf(line, sizeof line, "PRIORITY %d", prio);
		gfx_text(&fb, 12, 170, line, 3, 0xC0C0C0);
		bmp.hdr.Format = LGLCD_BMP_FORMAT_QVGAx32;
		for (int i = 0; i < 320 * 240; i++) { /* 0x00RRGGBB -> B G R A */
			bmp.pixels[i * 4] = (BYTE)px[i];
			bmp.pixels[i * 4 + 1] = (BYTE)(px[i] >> 8);
			bmp.pixels[i * 4 + 2] = (BYTE)(px[i] >> 16);
			bmp.pixels[i * 4 + 3] = 0xFF;
		}
		st = lgLcdUpdateBitmap(oc.device, &bmp.hdr, LGLCD_SYNC_UPDATE((DWORD)prio));
		if (st && st != 5)
			fprintf(stderr, "tdx-lcd-demo: update failed (%u)\n", (unsigned)st);
		usleep(250 * 1000);
	}
	lgLcdClose(oc.device);
	lgLcdDisconnect(cc.connection);
	lgLcdDeInit();
	return 0;
}
