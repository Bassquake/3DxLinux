#include "lcdmgr_core.h"
#include "lcd.h"

static int showable(const struct lm_cand *c)
{
	return c->has_frame && c->priority > 0;
}

int lm_pick(struct lm_sched *s, const struct lm_cand *c, int n, int algorithm, int delay_s,
	    int alerts, int manual, uint64_t now)
{
	int pick = -1;
	if (n <= 0) {
		s->shown = -1;
		return -1;
	}
	/* 1. alerts (newest wins) */
	if (alerts) {
		uint64_t best = 0;
		for (int i = 0; i < n; i++)
			if (c[i].has_frame && c[i].priority >= 255 && c[i].alert_time >= best) {
				best = c[i].alert_time;
				pick = i;
			}
	}
	/* 2. foreground requests (newest wins) */
	if (pick < 0) {
		uint64_t best = 0;
		for (int i = 0; i < n; i++)
			if (c[i].has_frame && c[i].foreground && c[i].fg_time >= best) {
				best = c[i].fg_time;
				pick = i;
			}
	}
	/* 3. switching algorithm */
	if (pick < 0 && algorithm == 1) {
		for (int i = 0; i < n; i++)
			if (c[i].prog == manual && c[i].has_frame)
				pick = i;
		if (pick < 0) /* nothing selected yet: first showable program */
			for (int i = 0; i < n && pick < 0; i++)
				if (showable(&c[i]))
					pick = i;
	} else if (pick < 0) {
		if (s->rot < 0 || s->rot >= n)
			s->rot = 0;
		if (!s->rot_since)
			s->rot_since = now;
		if (now - s->rot_since >= (uint64_t)(delay_s > 0 ? delay_s : 8) * 1000) {
			s->rot_since = now;
			for (int k = 1; k <= n; k++) /* next showable program */
				if (showable(&c[(s->rot + k) % n])) {
					s->rot = (s->rot + k) % n;
					break;
				}
		}
		if (showable(&c[s->rot]))
			pick = s->rot;
		else
			for (int k = 1; k <= n && pick < 0; k++)
				if (showable(&c[(s->rot + k) % n]))
					pick = s->rot = (s->rot + k) % n;
	}
	/* 4. last resort: anything with a frame */
	for (int i = 0; i < n && pick < 0; i++)
		if (c[i].has_frame)
			pick = i;
	s->shown = pick;
	return pick;
}

int lm_menu_buttons(struct lm_menu *m, uint32_t buttons, int nitems, int current, int quickswitch,
		    uint64_t now, int *commit)
{
	uint32_t pressed = buttons & ~m->prev;
	m->prev = buttons;
	if (!m->open) {
		if (!(pressed & LM_KEY_LCD))
			return (buttons & LM_KEY_LCD) ? LM_MENU_NONE : LM_MENU_FORWARD;
		if (nitems <= 0)
			return LM_MENU_NONE;
		m->last_input = now;
		if (quickswitch) { /* "Enable Quick Switch to Next Program Using Device" */
			*commit = current < 0 ? 0 : (current + 1) % nitems;
			return LM_MENU_COMMIT;
		}
		m->open = 1;
		m->sel = current < 0 || current >= nitems ? 0 : current;
		return LM_MENU_REDRAW;
	}
	if (!pressed)
		return LM_MENU_NONE;
	m->last_input = now;
	if (nitems <= 0) {
		m->open = 0;
		return LM_MENU_REDRAW;
	}
	if (pressed & TDX_SB_OK) {
		m->open = 0;
		*commit = m->sel;
		return LM_MENU_COMMIT;
	}
	if (pressed & TDX_SB_CANCEL) {
		m->open = 0;
		return LM_MENU_REDRAW;
	}
	if (pressed & (TDX_SB_UP | TDX_SB_LEFT)) {
		m->sel = (m->sel + nitems - 1) % nitems;
		return LM_MENU_REDRAW;
	}
	if (pressed & (TDX_SB_DOWN | TDX_SB_RIGHT | LM_KEY_LCD)) {
		m->sel = (m->sel + 1) % nitems;
		return LM_MENU_REDRAW;
	}
	return LM_MENU_NONE;
}

int lm_menu_tick(struct lm_menu *m, uint64_t now)
{
	if (m->open && now - m->last_input > LM_MENU_TIMEOUT_MS) {
		m->open = 0;
		return 1;
	}
	return 0;
}
