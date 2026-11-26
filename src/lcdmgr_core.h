/*
 * 3dxlinux-lcdmgr decisions, kept free of I/O so they can be unit-tested:
 * which program the LCD shows, and the on-LCD program menu.
 *
 * Display selection (LCDMon "Display Switching Method"):
 *   - an ALERT-priority (255) frame is shown at once if Alerts=1
 *     ("Immediate Display of High Priority Items");
 *   - a program that called lgLcdSetAsLCDForegroundApp(yes) is shown if its
 *     L flag allows it ("Allow Program to Force Itself to Foreground");
 *   - Algorithm 1 (manual): the program picked on the device (ManualModeApplet);
 *   - Algorithm 2/3 (rotate; Klever-VU is mono-only, treated as rotate here):
 *     the next program every Delay seconds.
 *   Programs with priority IDLE_NO_SHOW (0) are skipped unless nothing else is.
 *
 * Soft buttons (LCDMon FUN_14007fadc / FUN_140084a10 / FUN_14007fe10): the LCD
 * key (EDMBTDD bit 0, 0x80000000) opens the program list (or, with QuickSwitch,
 * switches straight to the next program); in the list UP/DOWN move, OK runs the
 * highlighted program, CANCEL closes, and 2 s without input closes it. Other
 * buttons go to the program being shown.
 */
#ifndef TDX_LCDMGR_CORE_H
#define TDX_LCDMGR_CORE_H

#include <stdint.h>

struct lm_cand {
	int prog;          /* program index (lm_db) */
	int has_frame;
	int priority;      /* last update's priority, 0..255 */
	int foreground;    /* asked to be foreground and allowed (L flag) */
	uint64_t fg_time;  /* when it asked */
	uint64_t alert_time;
};

struct lm_sched {
	int rot;           /* rotation position (index into candidates) */
	uint64_t rot_since;
	int shown;         /* candidate index shown last, -1 none */
};

/* Pick the candidate to show (or -1). @manual: program index selected on the device. */
int lm_pick(struct lm_sched *s, const struct lm_cand *c, int n, int algorithm, int delay_s,
	    int alerts, int manual, uint64_t now_ms);

enum lm_menu_result {
	LM_MENU_FORWARD = 0, /* not for the menu: send the buttons to the program */
	LM_MENU_NONE,        /* consumed, nothing to redraw */
	LM_MENU_REDRAW,      /* consumed, the menu changed (opened/moved/closed) */
	LM_MENU_COMMIT,      /* consumed, run item *commit (menu closed) */
};

struct lm_menu {
	int open;
	int sel;
	uint32_t prev;
	uint64_t last_input;
};

#define LM_MENU_TIMEOUT_MS 2000
#define LM_KEY_LCD 0x80000000u

int lm_menu_buttons(struct lm_menu *m, uint32_t buttons, int nitems, int current, int quickswitch,
		    uint64_t now_ms, int *commit);
/* Close the menu after LM_MENU_TIMEOUT_MS without input: returns 1 if it closed. */
int lm_menu_tick(struct lm_menu *m, uint64_t now_ms);

#endif
