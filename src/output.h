/* Event sinks: uinput virtual device and spacenavd-compatible UNIX socket. */
#ifndef TDX_OUTPUT_H
#define TDX_OUTPUT_H

#include "config.h"
#include "devices.h"

/* ---- uinput ---- */
struct tdx_uinput {
	int fd;
	int nbuttons;
};
int tdx_uinput_open(struct tdx_uinput *u, const struct tdx_device_def *dev,
		    const struct tdx_config *cfg);
void tdx_uinput_axes(struct tdx_uinput *u, const int axes[6]);
void tdx_uinput_button(struct tdx_uinput *u, int idx, int pressed);
void tdx_uinput_keys(struct tdx_uinput *u, const int *keys, int pressed);
void tdx_uinput_close(struct tdx_uinput *u);

/* ---- spacenavd v0 protocol server ---- */
#define TDX_SPNAV_MAX_CLIENTS 32
struct tdx_spnav {
	int lfd;
	int cfd[TDX_SPNAV_MAX_CLIENTS];
	char path[108];
};
int tdx_spnav_open(struct tdx_spnav *s, const char *path);
void tdx_spnav_accept(struct tdx_spnav *s);
void tdx_spnav_client_input(struct tdx_spnav *s, int idx);
void tdx_spnav_motion(struct tdx_spnav *s, const int axes[6], int period_ms);
void tdx_spnav_button(struct tdx_spnav *s, int bnum, int pressed);
void tdx_spnav_close(struct tdx_spnav *s);

#endif
