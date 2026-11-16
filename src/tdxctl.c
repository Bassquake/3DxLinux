/*
 * tdxctl - talk to a running tdxd.
 *
 *   tdxctl status              active profile, focused application, devices
 *   tdxctl profiles            list per-application profiles
 *   tdxctl focus APP [PID]     tell tdxd which application is focused
 *   tdxctl calibrate           re-zero every device (like 3DxWare's "Calibrate")
 *   tdxctl reload              re-read /etc/tdxd.conf and the profiles
 *   tdxctl -s SOCKET ...       use another control socket (default /run/tdxd.sock)
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
	fputs("usage: tdxctl [-s SOCKET] status | profiles | focus APP [PID] | calibrate | reload\n"
	      "       tdxctl -V\n", stderr);
	return 2;
}

int main(int argc, char **argv)
{
	const char *sock = "/run/tdxd.sock";
	int ai = 1;
	if (argc > 1 && (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))) {
		puts("tdxctl " TDX_VERSION);
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
	    strcmp(cmd, "calibrate") && strcmp(cmd, "reload"))
		return usage();
	if (!strcmp(cmd, "focus") && ai + 1 >= argc)
		return usage();

	char line[512] = "";
	for (int i = ai; i < argc && i < ai + 3; i++) {
		if (strchr(argv[i], '\n'))
			return usage();
		strncat(line, argv[i], sizeof line - strlen(line) - 2);
		if (i + 1 < argc && i + 1 < ai + 3)
			strncat(line, " ", sizeof line - strlen(line) - 1);
	}
	char reply[TDX_CTL_MAX];
	int rc = tdx_ctl_request(sock, line, reply, sizeof reply);
	if (rc) {
		fprintf(stderr, "tdxctl: cannot reach tdxd at %s: %s\n", sock, strerror(-rc));
		return 1;
	}
	fputs(reply, stdout);
	return strncmp(reply, "error", 5) ? 0 : 1;
}
