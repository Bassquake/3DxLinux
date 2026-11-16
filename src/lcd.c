#define _GNU_SOURCE
#include "lcd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>

#define VENDOR_OUT 0x41 /* host->device, vendor, interface */
#define VENDOR_IN  0xC1 /* device->host, vendor, interface */
#define REQ_BRIGHTNESS 0x0A
#define REQ_LEN 9
#define BULK_CHUNK 16384 /* multiple of 512: identical bus traffic to one write */

void tdx_lcd_header(uint8_t h[TDX_LCD_HDR])
{
	/* EDMBTDD.dll FUN_1800015a0 (+0x4b8 .. +0x6b7) */
	for (int i = 0; i < TDX_LCD_HDR; i++)
		h[i] = (uint8_t)i;      /* bytes 0x0f..0x1ff = index */
	h[0] = 0x10;
	h[1] = 0x0F;
	h[2] = 0x00; h[3] = 0x58; h[4] = 0x02; h[5] = 0x00; /* u32 0x25800 = pixel bytes */
	h[6] = 0x00;
	h[7] = 0x00; h[8] = 0x00;   /* x0 = 0   */
	h[9] = 0x00; h[10] = 0x00;  /* y0 = 0   */
	h[11] = 0x3F; h[12] = 0x01; /* x1 = 319 */
	h[13] = 0xEF; h[14] = 0x00; /* y1 = 239 */
}

void tdx_lcd_pack(const uint32_t *xrgb, uint8_t *out)
{
	/* EDMBTDD.dll FUN_180001c40: dst[x*240 + y] = RGB565(src[y*320 + x]) */
	for (int y = 0; y < TDX_LCD_H; y++) {
		for (int x = 0; x < TDX_LCD_W; x++) {
			uint32_t p = xrgb[y * TDX_LCD_W + x];
			uint16_t v = (uint16_t)((((p >> 19) & 0x1F) << 11) |
						(((p >> 10) & 0x3F) << 5) |
						((p >> 3) & 0x1F));
			size_t o = 2 * ((size_t)x * TDX_LCD_H + y);
			out[o] = (uint8_t)v;
			out[o + 1] = (uint8_t)(v >> 8);
		}
	}
}

static int read_hex_file(const char *path, unsigned *val)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int ok = fscanf(f, "%x", val) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static int read_dec_file(const char *path, unsigned *val)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int ok = fscanf(f, "%u", val) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

/* Find /dev/bus/usb/BBB/DDD for vid:pid via sysfs. */
static int find_usb_node(uint16_t vid, uint16_t pid, char *out, size_t outlen)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	if (!d)
		return -errno;
	struct dirent *e;
	int rc = -ENODEV;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || strchr(e->d_name, ':'))
			continue;
		char p[512];
		unsigned v, q, bus, dev;
		snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
		if (read_hex_file(p, &v) || v != vid)
			continue;
		snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
		if (read_hex_file(p, &q) || q != pid)
			continue;
		snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/busnum", e->d_name);
		if (read_dec_file(p, &bus))
			continue;
		snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/devnum", e->d_name);
		if (read_dec_file(p, &dev))
			continue;
		snprintf(out, outlen, "/dev/bus/usb/%03u/%03u", bus, dev);
		rc = 0;
		break;
	}
	closedir(d);
	return rc;
}

/* Walk raw descriptors (usbfs read) to find the first bulk OUT endpoint of @iface. */
static int find_bulk_out(int fd, int iface, unsigned *ep)
{
	uint8_t buf[4096];
	ssize_t n = pread(fd, buf, sizeof buf, 0);
	if (n < 18)
		return -EIO;
	int cur_iface = -1;
	for (ssize_t i = 0; i + 2 <= n && buf[i] >= 2; i += buf[i]) {
		uint8_t len = buf[i], type = buf[i + 1];
		if (i + len > n)
			break;
		if (type == 4 && len >= 9) /* INTERFACE */
			cur_iface = buf[i + 2];
		else if (type == 5 && len >= 7 && cur_iface == iface) { /* ENDPOINT */
			uint8_t addr = buf[i + 2], attr = buf[i + 3];
			if (!(addr & 0x80) && (attr & 3) == 2) {
				*ep = addr;
				return 0;
			}
		}
	}
	return -ENOENT;
}

int tdx_lcd_open(struct tdx_lcd *lcd, uint16_t vid, uint16_t pid)
{
	lcd->fd = -1;
	lcd->iface = 0;
	int rc = find_usb_node(vid, pid, lcd->path, sizeof lcd->path);
	if (rc)
		return rc;
	lcd->fd = open(lcd->path, O_RDWR | O_CLOEXEC);
	if (lcd->fd < 0)
		return -errno;
	if ((rc = find_bulk_out(lcd->fd, lcd->iface, &lcd->ep_out))) {
		lcd->ep_out = 0x02; /* G19-family default */
	}
	/* Detach any kernel driver bound to the vendor interface (normally none). */
	struct usbdevfs_ioctl cmd = { .ifno = lcd->iface, .ioctl_code = USBDEVFS_DISCONNECT };
	ioctl(lcd->fd, USBDEVFS_IOCTL, &cmd);
	unsigned int ifno = (unsigned)lcd->iface;
	if (ioctl(lcd->fd, USBDEVFS_CLAIMINTERFACE, &ifno) < 0) {
		rc = -errno;
		close(lcd->fd);
		lcd->fd = -1;
		return rc;
	}
	tdx_lcd_header(lcd->frame);
	return 0;
}

void tdx_lcd_close(struct tdx_lcd *lcd)
{
	if (lcd->fd < 0)
		return;
	unsigned int ifno = (unsigned)lcd->iface;
	ioctl(lcd->fd, USBDEVFS_RELEASEINTERFACE, &ifno);
	close(lcd->fd);
	lcd->fd = -1;
}

int tdx_lcd_show(struct tdx_lcd *lcd, const uint32_t *xrgb)
{
	if (lcd->fd < 0)
		return -EBADF;
	tdx_lcd_pack(xrgb, lcd->frame + TDX_LCD_HDR);
	for (size_t off = 0; off < TDX_LCD_FRAME;) {
		size_t n = TDX_LCD_FRAME - off;
		if (n > BULK_CHUNK)
			n = BULK_CHUNK;
		struct usbdevfs_bulktransfer bt = {
			.ep = lcd->ep_out, .len = (unsigned)n, .timeout = 1000,
			.data = lcd->frame + off,
		};
		int r = ioctl(lcd->fd, USBDEVFS_BULK, &bt);
		if (r < 0)
			return -errno;
		off += (size_t)r;
		if (r == 0)
			return -EIO;
	}
	return 0;
}

static int vendor_ctrl(struct tdx_lcd *lcd, uint8_t type, uint8_t req, uint8_t *data, uint16_t len)
{
	struct usbdevfs_ctrltransfer ct = {
		.bRequestType = type, .bRequest = req, .wValue = 0,
		.wIndex = (uint16_t)lcd->iface, .wLength = len, .timeout = 1000, .data = data,
	};
	int r = ioctl(lcd->fd, USBDEVFS_CONTROL, &ct);
	return r < 0 ? -errno : r;
}

int tdx_lcd_set_brightness(struct tdx_lcd *lcd, int percent)
{
	if (lcd->fd < 0)
		return -EBADF;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	uint8_t d[REQ_LEN] = {0};
	d[0] = (uint8_t)((percent + 1) & 0x7F); /* wire = UI value + 1 (EDMBTDD) */
	int r = vendor_ctrl(lcd, VENDOR_OUT, REQ_BRIGHTNESS, d, REQ_LEN);
	return r < 0 ? r : 0;
}

int tdx_lcd_get_brightness(struct tdx_lcd *lcd)
{
	if (lcd->fd < 0)
		return -EBADF;
	uint8_t d[REQ_LEN] = {0};
	int r = vendor_ctrl(lcd, VENDOR_IN, REQ_BRIGHTNESS, d, REQ_LEN);
	if (r < 1)
		return r < 0 ? r : -EIO;
	return (d[0] & 0x7F) - 1;
}
