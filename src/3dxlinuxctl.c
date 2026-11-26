/*
 * 3dxlinuxctl - talk to a running 3dxlinuxd.
 *
 *   3dxlinuxctl status              active profile, focused application, devices
 *   3dxlinuxctl profiles            list per-application profiles
 *   3dxlinuxctl focus APP [PID]     tell 3dxlinuxd which application is focused
 *   3dxlinuxctl calibrate           re-zero every device (like 3DxWare's "Calibrate")
 *   3dxlinuxctl reload              re-read /etc/3dxlinux.conf and the profiles
 *   3dxlinuxctl buttons             each key's number (the spacenav button) and V3DK id
 *   3dxlinuxctl app-keys            the names the application gave its keys (app-buttons)
 *   3dxlinuxctl app-buttons PID [V3DK NAME]...
 *                              the keys application PID handles itself, with its
 *                              names for the LCD (none: give them back to 3dxlinuxd)
 *   3dxlinuxctl keys KEY_A+KEY_B    type a key combination (the radial menu's, the NumPad's)
 *   3dxlinuxctl action NAME         do a button function: speed_up, settings, numpad ...
 *   3dxlinuxctl radial ID           a radial menu of your own, as 3dxlinux-radial reads it
 *   3dxlinuxctl -s SOCKET ...       use another control socket (default /run/3dxlinux.sock)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "control.h"

#ifndef TDX_VERSION
#define TDX_VERSION "unknown"
#endif

static int usage(void)
{
	fputs("usage: 3dxlinuxctl [-s SOCKET] status | profiles | focus APP [PID] | calibrate | reload\n"
	      "       3dxlinuxctl [-s SOCKET] buttons | app-keys | app-buttons PID [V3DK NAME]...\n"
	      "       3dxlinuxctl [-s SOCKET] pair [mouse|spacemouse|spacemousepro] | pair-stop | unpair SLOT\n"
	      "       3dxlinuxctl [-s SOCKET] keys KEY_A+KEY_B | action NAME | radial ID\n"
	      "       3dxlinuxctl -V\n", stderr);
	return 2;
}

int main(int argc, char **argv)
{
	const char *sock = "/run/3dxlinux.sock";
	int ai = 1;
	if (argc > 1 && (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))) {
		puts("3dxlinuxctl " TDX_VERSION);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "-s")) {
		sock = argv[2];
		ai = 3;
	}
	if (ai >= argc)
		return usage();
	const char *cmd = argv[ai];
	if (strcmp(cmd, "status") && strcmp(cmd, "profiles") && strcmp(cmd, "focus") &&
	    strcmp(cmd, "calibrate") && strcmp(cmd, "reload") && strcmp(cmd, "buttons") &&
	    strcmp(cmd, "app-buttons") && strcmp(cmd, "app-keys") && strcmp(cmd, "pair") &&
	    strcmp(cmd, "pair-stop") && strcmp(cmd, "unpair") && strcmp(cmd, "keys") &&
	    strcmp(cmd, "action") && strcmp(cmd, "radial"))
		return usage();
	if ((!strcmp(cmd, "focus") || !strcmp(cmd, "app-buttons") || !strcmp(cmd, "unpair") ||
	     !strcmp(cmd, "keys") || !strcmp(cmd, "action") || !strcmp(cmd, "radial")) &&
	    ai + 1 >= argc)
		return usage();

	/* app-buttons takes every field, split by tabs: the names have spaces */
	int all = !strcmp(cmd, "app-buttons");
	int last = all ? argc : ai + 3;
	char line[2048] = "";
	for (int i = ai; i < argc && i < last; i++) {
		if (strchr(argv[i], '\n') || (all && strchr(argv[i], '\t')))
			return usage();
		strncat(line, argv[i], sizeof line - strlen(line) - 2);
		if (i + 1 < argc && i + 1 < last)
			strncat(line, all ? "\t" : " ", sizeof line - strlen(line) - 1);
	}
	char reply[TDX_CTL_MAX];
	int rc = tdx_ctl_request(sock, line, reply, sizeof reply);
	if (rc) {
		fprintf(stderr, "3dxlinuxctl: cannot reach 3dxlinuxd at %s: %s\n", sock, strerror(-rc));
		return 1;
	}
	fputs(reply, stdout);
	return strncmp(reply, "error", 5) ? 0 : 1;
}
