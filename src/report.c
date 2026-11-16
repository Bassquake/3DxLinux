#include "report.h"
#include <string.h>

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

int tdx_led_report(const struct tdx_device_def *dev, int on, uint8_t out[2])
{
	if (dev && dev->pid == 0xC63C)
		return 0;
	out[0] = 0x04;
	out[1] = on ? 1 : 0;
	return 2;
}

int tdx_has_rgb_leds(const struct tdx_device_def *dev)
{
	return dev && dev->pid == 0xC63C;
}

void tdx_rgb_report(uint8_t index, uint32_t rgb, uint8_t out[8])
{
	out[0] = TDX_RGB_REPORT_ID;
	out[1] = TDX_RGB_CMD_COLOR;
	out[2] = index;
	out[3] = (uint8_t)(rgb >> 16);
	out[4] = (uint8_t)(rgb >> 8);
	out[5] = (uint8_t)rgb;
	out[6] = 0;
	out[7] = 0;
}

int tdx_lcd_intensity_report(const struct tdx_device_def *dev, int percent, uint8_t out[2])
{
	if (!dev || dev->pid != 0xC633 || percent < 0)
		return 0;
	if (percent > 100)
		percent = 100;
	out[0] = TDX_LCD_INTENSITY_REPORT_ID;
	out[1] = (uint8_t)percent;
	return 2;
}

int tdx_button_count(const struct tdx_device_def *dev)
{
	if (dev && dev->nbuttons)
		return dev->nbuttons > TDX_MAX_BUTTONS ? TDX_MAX_BUTTONS : dev->nbuttons;
	return 32;
}

static int number_pressed(const struct tdx_state *st, uint32_t n)
{
	for (int i = 0; i < 6; i++)
		if (st->numbers1c[i] && st->numbers1c[i] == n)
			return 1;
	return 0;
}

/* Recompute logical button states from raw masks; returns 1 if changed. */
static int resolve_buttons(struct tdx_state *st, const struct tdx_device_def *dev)
{
	uint8_t nb[TDX_MAX_BUTTONS] = {0};
	int n = tdx_button_count(dev);

	if (!dev || !dev->nbuttons) {
		for (int i = 0; i < n; i++)
			nb[i] = (st->mask03 >> i) & 1;
	} else {
		for (int i = 0; i < n; i++) {
			const struct tdx_button_def *b = &dev->buttons[i];
			switch (b->kind) {
			case TDX_BTN_MASK:
				nb[i] = (b->report_id == 0x03) ? !!(st->mask03 & b->value) : 0;
				break;
			case TDX_BTN_NUMBER:
				nb[i] = number_pressed(st, b->value);
				break;
			case TDX_BTN_LONGPRESS:
				nb[i] = !!(st->lp1d & b->lp_mask);
				break;
			}
		}
	}
	int changed = memcmp(nb, st->btn, sizeof nb) != 0;
	memcpy(st->btn, nb, sizeof nb);
	return changed;
}

int tdx_report_feed(struct tdx_state *st, const struct tdx_device_def *dev,
		    const uint8_t *buf, size_t len)
{
	int r = 0;
	if (len < 1)
		return 0;

	switch (buf[0]) {
	case 0x01:
		if (len >= 7) {
			for (int i = 0; i < 3; i++)
				st->axis[i] = rd16(buf + 1 + 2 * i);
			st->axis_dirty |= 1;
			r |= 1;
		}
		if (len >= 13) { /* combined translation+rotation report */
			for (int i = 0; i < 3; i++)
				st->axis[3 + i] = rd16(buf + 7 + 2 * i);
			st->axis_dirty |= 2;
		}
		break;
	case 0x02:
		if (len >= 7) {
			for (int i = 0; i < 3; i++)
				st->axis[3 + i] = rd16(buf + 1 + 2 * i);
			st->axis_dirty |= 2;
			r |= 1;
		}
		break;
	case 0x03: {
		uint32_t m = 0;
		for (size_t i = 1; i < len && i <= 4; i++)
			m |= (uint32_t)buf[i] << (8 * (i - 1));
		st->mask03 = m;
		if (resolve_buttons(st, dev))
			r |= 2;
		break;
	}
	case 0x1C:
		memset(st->numbers1c, 0, sizeof st->numbers1c);
		for (int i = 0; i < 6 && 1 + 2 * i + 1 < (int)len; i++)
			st->numbers1c[i] = (uint16_t)rd16(buf + 1 + 2 * i);
		if (resolve_buttons(st, dev))
			r |= 2;
		break;
	case 0x1D: {
		uint32_t m = 0;
		for (size_t i = 1; i < len && i <= 4; i++)
			m |= (uint32_t)buf[i] << (8 * (i - 1));
		st->lp1d = m;
		if (resolve_buttons(st, dev))
			r |= 2;
		break;
	}
	default:
		break;
	}
	return r;
}
