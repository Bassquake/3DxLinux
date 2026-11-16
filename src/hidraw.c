#define _GNU_SOURCE
#include "hidraw.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct tdx_device_def *tdx_device_lookup(uint16_t vid, uint16_t pid)
{
	for (size_t i = 0; i < tdx_device_count; i++)
		if (tdx_devices[i].vid == vid && tdx_devices[i].pid == pid)
			return &tdx_devices[i];
	return NULL;
}

/* Parse /sys/class/hidraw/X/device/uevent:
 *   HID_ID=0003:0000046D:0000C629
 *   HID_PHYS=usb-0000:00:14.0-2/input1
 */
static int parse_uevent(const char *path, struct tdx_hid_node *n)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[256];
	int got = 0;
	n->iface = -1;
	while (fgets(line, sizeof line, f)) {
		unsigned bus, vid, pid;
		if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) {
			n->bus = (int)bus;
			n->vid = (uint16_t)vid;
			n->pid = (uint16_t)pid;
			got = 1;
		} else if (!strncmp(line, "HID_PHYS=", 9)) {
			char *p = strstr(line, "/input");
			if (p)
				n->iface = atoi(p + 6);
		}
	}
	fclose(f);
	return got ? 0 : -1;
}

int tdx_hidraw_scan(struct tdx_hid_node *out, int max)
{
	DIR *d = opendir("/sys/class/hidraw");
	if (!d)
		return 0;
	int cnt = 0;
	struct dirent *e;
	while ((e = readdir(d)) && cnt < max) {
		if (strncmp(e->d_name, "hidraw", 6))
			continue;
		char p[300];
		struct tdx_hid_node n;
		memset(&n, 0, sizeof n);
		snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", e->d_name);
		if (parse_uevent(p, &n))
			continue;
		n.def = tdx_device_lookup(n.vid, n.pid);
		if (!n.def || n.def->dev_class == TDX_CLASS_2DMOUSE ||
		    n.def->dev_class == TDX_CLASS_KEYBOARD)
			continue; /* 2D mice / keyboards work natively via hid-generic */
		/* Multi-interface USB devices: only the collection the INF names. */
		if (n.bus == 3 && n.def->hid_iface && n.iface >= 0 && n.iface != n.def->hid_iface)
			continue;
		snprintf(n.devnode, sizeof n.devnode, "/dev/%s", e->d_name);
		out[cnt++] = n;
	}
	closedir(d);
	return cnt;
}
