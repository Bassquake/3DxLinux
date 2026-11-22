#include "receiver.h"

#include <stdio.h>
#include <string.h>

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
	return 1;
}

int tdx_rcv_parse_battery(const uint8_t *buf, size_t n, int *level, int *charging)
{
	if (n < 3 || buf[0] != TDX_RCV_REPORT_BATTERY)
		return 0;
	*level = buf[1] ? (buf[1] > 100 ? 100 : buf[1]) : -1;
	*charging = buf[2] & 1;
	return 1;
}
