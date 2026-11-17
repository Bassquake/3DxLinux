#define _GNU_SOURCE
#include "lcd.h"
#include "usbfs.h"

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
#define REQ_PING 0x01
#define REQ_BRIGHTNESS 0x0A
#define REQ_LEN 9
#define READY_SETTLE_MS 500 /* EDMBTDD sleeps 500 ms after the first successful ping */
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
int tdx_usb_find_node(uint16_t vid, uint16_t pid, char *out, size_t outlen)
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
int tdx_usb_find_bulk_out(int fd, int iface, unsigned *ep)
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

int tdx_usb_find_vendor_iface(const char *path, int *iface)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	uint8_t buf[4096];
	ssize_t n = pread(fd, buf, sizeof buf, 0);
	close(fd);
	int cur = -1, vendor = 0;
	for (ssize_t i = 0; n > 0 && i + 2 <= n && buf[i] >= 2 && i + buf[i] <= n; i += buf[i]) {
		uint8_t len = buf[i], type = buf[i + 1];
		if (type == 4 && len >= 9) { /* INTERFACE: [2] number, [5] class */
			cur = buf[i + 2];
			vendor = buf[i + 5] == 0xFF;
		} else if (type == 5 && len >= 7 && vendor && !(buf[i + 2] & 0x80) &&
			   (buf[i + 3] & 3) == 2) {
			*iface = cur;
			return 0;
		}
	}
	return -ENOENT;
}

int tdx_usb_claim(const char *path, int iface)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	/* Detach any kernel driver bound to the vendor interface (normally none). */
	struct usbdevfs_ioctl cmd = { .ifno = iface, .ioctl_code = USBDEVFS_DISCONNECT };
	ioctl(fd, USBDEVFS_IOCTL, &cmd);
	unsigned int ifno = (unsigned)iface;
	if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifno) < 0) {
		int rc = -errno;
		close(fd);
		return rc;
	}
	return fd;
}

void tdx_usb_release(int fd, int iface)
{
	if (fd < 0)
		return;
	unsigned int ifno = (unsigned)iface;
	ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifno);
	close(fd);
}

int tdx_usb_bulk_out(int fd, unsigned ep, const uint8_t *buf, size_t len, unsigned timeout_ms)
{
	for (size_t off = 0; off < len;) {
		size_t n = len - off;
		if (n > BULK_CHUNK)
			n = BULK_CHUNK;
		struct usbdevfs_bulktransfer bt = {
			.ep = ep, .len = (unsigned)n, .timeout = timeout_ms,
			.data = (void *)(buf + off),
		};
		int r = ioctl(fd, USBDEVFS_BULK, &bt);
		if (r < 0)
			return -errno;
		if (r == 0)
			return -EIO;
		off += (size_t)r;
	}
	return 0;
}

int tdx_lcd_open(struct tdx_lcd *lcd, uint16_t vid, uint16_t pid)
{
	lcd->fd = -1;
	lcd->iface = 0;
	lcd->ep_in = 0;
	lcd->in_len = 0;
	lcd->in_pending = 0;
	int rc = tdx_usb_find_node(vid, pid, lcd->path, sizeof lcd->path);
	if (rc)
		return rc;
	int fd = tdx_usb_claim(lcd->path, lcd->iface);
	if (fd < 0)
		return fd;
	lcd->fd = fd;
	if (tdx_usb_find_bulk_out(lcd->fd, lcd->iface, &lcd->ep_out))
		lcd->ep_out = 0x02; /* G19-family default */
	tdx_lcd_header(lcd->frame);
	return 0;
}

void tdx_lcd_close(struct tdx_lcd *lcd)
{
	if (lcd->fd >= 0 && lcd->in_pending) {
		ioctl(lcd->fd, USBDEVFS_DISCARDURB, &lcd->in_urb);
		void *done = NULL;
		ioctl(lcd->fd, USBDEVFS_REAPURBNDELAY, &done);
		lcd->in_pending = 0;
	}
	tdx_usb_release(lcd->fd, lcd->iface);
	lcd->fd = -1;
}

uint32_t tdx_lcd_softbuttons(uint8_t b)
{
	uint32_t m = 0;
	if (b & 0x80) m |= TDX_SB_UP;
	if (b & 0x40) m |= TDX_SB_DOWN;
	if (b & 0x20) m |= TDX_SB_LEFT;
	if (b & 0x10) m |= TDX_SB_RIGHT;
	if (b & 0x08) m |= TDX_SB_OK;
	if (b & 0x04) m |= TDX_SB_MENU;
	if (b & 0x02) m |= TDX_SB_CANCEL;
	if (b & 0x01) m |= TDX_SB_SPECIAL;
	return m;
}

/* First interrupt IN endpoint of @iface and its wMaxPacketSize. */
static int find_int_in(int fd, int iface, unsigned *ep, unsigned *maxpkt)
{
	uint8_t buf[4096];
	ssize_t n = pread(fd, buf, sizeof buf, 0);
	int cur = -1;
	for (ssize_t i = 0; n > 0 && i + 2 <= n && buf[i] >= 2 && i + buf[i] <= n; i += buf[i]) {
		if (buf[i + 1] == 4 && buf[i] >= 9)
			cur = buf[i + 2];
		else if (buf[i + 1] == 5 && buf[i] >= 7 && cur == iface && (buf[i + 2] & 0x80) &&
			 (buf[i + 3] & 3) == 3) {
			*ep = buf[i + 2];
			*maxpkt = (unsigned)(buf[i + 4] | buf[i + 5] << 8) & 0x7FF;
			return 0;
		}
	}
	return -ENOENT;
}

int tdx_lcd_buttons_start(struct tdx_lcd *lcd)
{
	if (lcd->fd < 0)
		return -EBADF;
	if (lcd->in_pending)
		return 0;
	if (!lcd->ep_in && find_int_in(lcd->fd, lcd->iface, &lcd->ep_in, &lcd->in_len))
		return -ENOENT;
	/* exactly one packet, as LGPBTDD.sys FUN_00012434 reads (length = wMaxPacketSize):
	 * a longer request would wait for several full 2-byte packets */
	if (!lcd->in_len || lcd->in_len > sizeof lcd->in_buf)
		lcd->in_len = 2;
	memset(&lcd->in_urb, 0, sizeof lcd->in_urb);
	lcd->in_urb.type = USBDEVFS_URB_TYPE_INTERRUPT;
	lcd->in_urb.endpoint = (unsigned char)lcd->ep_in;
	lcd->in_urb.buffer = lcd->in_buf;
	lcd->in_urb.buffer_length = (int)lcd->in_len;
	if (ioctl(lcd->fd, USBDEVFS_SUBMITURB, &lcd->in_urb) < 0)
		return -errno;
	lcd->in_pending = 1;
	return 0;
}

int tdx_lcd_buttons_reap(struct tdx_lcd *lcd, uint32_t *buttons)
{
	if (lcd->fd < 0)
		return -EBADF;
	struct usbdevfs_urb *done = NULL;
	if (ioctl(lcd->fd, USBDEVFS_REAPURBNDELAY, &done) < 0)
		return errno == EAGAIN ? 0 : -errno;
	if (done != &lcd->in_urb)
		return 0;
	lcd->in_pending = 0;
	int st = lcd->in_urb.status, got = lcd->in_urb.actual_length;
	if (st == -ENODEV || st == -ESHUTDOWN)
		return -ENODEV;
	int rc = 0;
	if (st == 0 && got >= 1) {
		*buttons = tdx_lcd_softbuttons(lcd->in_buf[0]);
		rc = 1;
	}
	int r = tdx_lcd_buttons_start(lcd);
	return r < 0 ? r : rc;
}

int tdx_lcd_show(struct tdx_lcd *lcd, const uint32_t *xrgb)
{
	if (lcd->fd < 0)
		return -EBADF;
	tdx_lcd_pack(xrgb, lcd->frame + TDX_LCD_HDR);
	return tdx_usb_bulk_out(lcd->fd, lcd->ep_out, lcd->frame, TDX_LCD_FRAME, 1000);
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

int tdx_lcd_ping(struct tdx_lcd *lcd)
{
	if (lcd->fd < 0)
		return -EBADF;
	uint8_t d[REQ_LEN] = {0};
	int r = vendor_ctrl(lcd, VENDOR_IN, REQ_PING, d, REQ_LEN);
	return r < 0 ? r : 0;
}

int tdx_lcd_wait_ready(struct tdx_lcd *lcd, int tries)
{
	/* EDMBTDD.dll FUN_180002530 (called from LCDExtDeviceManagerResumeReEnumerate):
	 * ping up to 100 times, 100 ms apart, then settle 500 ms. */
	int rc = -EIO;
	for (int i = 0; i < tries; i++) {
		rc = tdx_lcd_ping(lcd);
		if (rc == 0)
			break;
		if (rc == -ENODEV || rc == -EBADF)
			return rc;
		usleep(100 * 1000);
	}
	if (rc == 0)
		usleep(READY_SETTLE_MS * 1000);
	return rc;
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
