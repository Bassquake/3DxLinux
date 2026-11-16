#ifndef TDX_HIDRAW_H
#define TDX_HIDRAW_H

#include <stdint.h>
#include "devices.h"

struct tdx_hid_node {
	char devnode[288];   /* /dev/hidrawN */
	uint16_t vid, pid;
	int bus;            /* 3 = USB, 5 = Bluetooth */
	int iface;          /* USB interface number or -1 */
	const struct tdx_device_def *def;
};

/* Enumerate hidraw nodes that belong to known 3Dconnexion devices.
 * Returns number of entries written (<= max). */
int tdx_hidraw_scan(struct tdx_hid_node *out, int max);

#endif
