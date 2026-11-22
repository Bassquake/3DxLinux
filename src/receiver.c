#include "receiver.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <linux/input-event-codes.h>

static const struct {
	uint16_t code, pid;
	int cls;
} codes[] = {
	{'D', 0xC631, TDX_RCV_3DMOUSE},  /* SpaceMouse Pro Wireless */
	{'A', 0xC62E, TDX_RCV_3DMOUSE},  /* SpaceMouse Wireless */
	{'T', 0xC651, TDX_RCV_2DMOUSE},  /* CadMouse Wireless */
	{'U', 0xC654, TDX_RCV_2DMOUSE},  /* CadMouse Pro Wireless */
	{'V', 0xC657, TDX_RCV_2DMOUSE},  /* CadMouse Pro Wireless Left */
	{'Y', 0xC658, TDX_RCV_2DMOUSE},  /* CadMouse Compact Wireless */
	{'N', 0xC665, TDX_RCV_KEYBOARD}, /* Keyboard Pro */
	{'Q', 0xC638, TDX_RCV_3DMOUSE},
	{'Z', 0xC63A, TDX_RCV_3DMOUSE},
	{0x414B, 0xC664, TDX_RCV_KEYBOARD},
	{0x4355, 0xC669, TDX_RCV_KEYBOARD},
	{0x434B, 0xC665, TDX_RCV_KEYBOARD},
	{0x5255, 0xC652, TDX_RCV_RECEIVER}, /* "UR" */
};

void tdx_rcv_cmd_pairing(uint8_t out[5], int mode)
{
	memset(out, 0, 5);
	out[0] = TDX_RCV_REPORT_CONTROL;
	out[1] = 0x02;
	out[2] = (uint8_t)mode;
}

void tdx_rcv_cmd_unpair(uint8_t out[5], int slot)
{
	memset(out, 0, 5);
	out[0] = TDX_RCV_REPORT_CONTROL;
	out[1] = 0x04;
	out[2] = (uint8_t)slot;
}

uint16_t tdx_rcv_code_pid(uint16_t code, int *cls)
{
	for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
		if (codes[i].code == code) {
			if (cls)
				*cls = codes[i].cls;
			return codes[i].pid;
		}
	if (cls)
		*cls = 0;
	return 0;
}

int tdx_rcv_parse_slot(const uint8_t *buf, size_t n, struct tdx_rcv_slot *out)
{
	memset(out, 0, sizeof *out);
	if (n < 8 || buf[0] < TDX_RCV_REPORT_SELF || buf[0] >= TDX_RCV_REPORT_SLOT0 + TDX_RCV_SLOTS)
		return -1;
	out->code = (uint16_t)(buf[1] | buf[2] << 8);
	if (!out->code)
		return 0;
	out->pid = tdx_rcv_code_pid(out->code, &out->cls);
	snprintf(out->id, sizeof out->id, "%02x%02x%02x%02x%02x", buf[3], buf[4], buf[5], buf[6],
		 buf[7]);
	unsigned long long num = 0;
	for (int i = 3; i < 8; i++)
		num = num << 8 | buf[i];
	char pre[3] = {0};
	int np = 0;
	for (int i = 1; i <= 2; i++)
		if (buf[i] >= 0x20 && buf[i] < 0x7f)
			pre[np++] = (char)buf[i];
	snprintf(out->serial, sizeof out->serial, "%s%llu", pre, num);
	return 1;
}

const struct tdx_keymap *tdx_rcv_mouse_binding(const struct tdx_config *cfg,
					       const struct tdx_device_def *dev, int idx)
{
	if (!dev || idx >= dev->nbuttons || !dev->buttons[idx].v3dk)
		return NULL;
	for (int i = 0; i < cfg->nkeymap; i++)
		if (!strcasecmp(cfg->keymap[i].v3dk, dev->buttons[idx].v3dk))
			return cfg->keymap[i].keys[0] ? &cfg->keymap[i] : NULL;
	return NULL;
}

static int mouse_fn(int code)
{
	switch (code) {
	case BTN_LEFT: return 1;
	case BTN_RIGHT: return 2;
	case BTN_MIDDLE: return 3;
	case BTN_SIDE: return 4;  /* back */
	case BTN_EXTRA: return 5; /* forward */
	}
	return 0;
}

unsigned tdx_rcv_mouse_map(const struct tdx_config *cfg, const struct tdx_device_def *dev,
			   uint8_t map[7])
{
	/* 3DxWare's defaults: left, right, middle, wheel = middle, forward, back,
	 * radial menu (reports) */
	static const int def[7] = {1, 2, 3, 3, 5, 4, 0};
	unsigned vendor = 0;
	for (int i = 0; i < 7; i++) {
		int fn = def[i];
		const struct tdx_keymap *km = i >= 2 ? tdx_rcv_mouse_binding(cfg, dev, i) : NULL;
		if (km)
			fn = !km->keys[1] ? mouse_fn(km->keys[0]) : 0;
		map[i] = (uint8_t)(fn ? 9 + fn : 40 + i + 1);
		if (!fn)
			vendor |= 1u << i;
	}
	if (cfg->mouse_swap_buttons) { /* as 3DxWare: right and middle trade places */
		uint8_t t = map[1];
		map[1] = map[2];
		map[2] = t;
	}
	return vendor;
}

void tdx_rcv_mouse_config(uint8_t rep[TDX_RCV_MOUSE_CONFIG_LEN], int dpi, int rate_hz, int smart,
			  const uint8_t map[7])
{
	uint8_t *b = rep + 1;
	memset(rep, 0, TDX_RCV_MOUSE_CONFIG_LEN);
	rep[0] = TDX_RCV_REPORT_MOUSE_CONFIG;
	b[1] = dpi < 51 ? 1 : dpi < 0x2008 ? (uint8_t)(dpi / 50) : 0xA4;
	b[2] = 0; /* lift detection on, threshold 0 (3DxWare's default) */
	if (smart)
		b[6] = 1;
	else {
		b[3] = 0x01;
		b[4] = 0xFF;
	}
	memcpy(b + 18, map, 7);
	b[26] = 0x1E;
	b[30] = rate_hz > 500 ? 1 : rate_hz > 250 ? 2 : rate_hz > 125 ? 4 : 8;
}

int tdx_rcv_parse_battery(const uint8_t *buf, size_t n, int *level, int *charging)
{
	if (n < 3 || buf[0] != TDX_RCV_REPORT_BATTERY)
		return 0;
	*level = buf[1] ? (buf[1] > 100 ? 100 : buf[1]) : -1;
	*charging = buf[2] & 1;
	return 1;
}
