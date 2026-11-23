/* Event sinks: uinput virtual device and spacenavd-compatible UNIX socket. */
#ifndef TDX_OUTPUT_H
#define TDX_OUTPUT_H

#include "config.h"
#include "devices.h"

/* ---- uinput ---- */
/* Two virtual devices: the 3D mouse (6 ABS axes + its buttons) and, apart, a
 * keyboard for the keys that bindings send. In one device the keyboard keys
 * made libinput take it for an absolute pointer instead of ignoring it as a
 * joystick, so the cursor jumped around whenever the cap moved. */
struct tdx_uinput {
	int fd;   /* 3D mouse */
	int kfd;  /* keyboard, -1 when no key bindings */
	int nbuttons;
	int numlock; /* the desktop's Num Lock (its LED on our keyboard): -1 not known */
};
int tdx_uinput_open(struct tdx_uinput *u, const struct tdx_device_def *dev,
		    const struct tdx_config *cfg);
void tdx_uinput_axes(struct tdx_uinput *u, const int axes[6]);
void tdx_uinput_button(struct tdx_uinput *u, int idx, int pressed);
void tdx_uinput_keys(struct tdx_uinput *u, const struct tdx_keymap *km, int pressed);
/* Num Lock on the keyboard device: 1 on, 0 off, -1 not known (the desktop
 * sets the LEDs of every keyboard; uinput hands them to us). */
int tdx_uinput_numlock(struct tdx_uinput *u);
/* Only the keyboard device (a CadMouse's keystroke buttons); fd stays -1. */
int tdx_uinput_open_keys(struct tdx_uinput *u, const struct tdx_device_def *dev,
			 const struct tdx_config *cfg);
void tdx_uinput_close(struct tdx_uinput *u);

/* ---- spacenavd v0 protocol server ---- */
#define TDX_SPNAV_MAX_CLIENTS 32
struct tdx_spnav {
	int lfd;
	int cfd[TDX_SPNAV_MAX_CLIENTS];
	char app[TDX_SPNAV_MAX_CLIENTS][64]; /* client process name (SO_PEERCRED) */
	long long seq[TDX_SPNAV_MAX_CLIENTS]; /* connection order */
	long long next_seq;
	char path[108];
};
/* Application of the most recently connected client still connected, or NULL. */
const char *tdx_spnav_latest_app(const struct tdx_spnav *s);
int tdx_spnav_open(struct tdx_spnav *s, const char *path);
void tdx_spnav_accept(struct tdx_spnav *s);
void tdx_spnav_client_input(struct tdx_spnav *s, int idx);
void tdx_spnav_motion(struct tdx_spnav *s, const int axes[6], int period_ms);
void tdx_spnav_button(struct tdx_spnav *s, int bnum, int pressed);
void tdx_spnav_close(struct tdx_spnav *s);

#endif
