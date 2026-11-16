/* Minimal Linux usbfs helpers shared by the LCD drivers (no libusb). */
#ifndef TDX_USBFS_H
#define TDX_USBFS_H

#include <stddef.h>
#include <stdint.h>

/* /dev/bus/usb/BBB/DDD for vid:pid via sysfs. 0 or -errno. */
int tdx_usb_find_node(uint16_t vid, uint16_t pid, char *out, size_t outlen);
/* First bulk OUT endpoint of interface @iface (raw descriptor walk). */
int tdx_usb_find_bulk_out(int fd, int iface, unsigned *ep);
/* Open @path, detach any kernel driver from @iface and claim it. Returns fd or -errno. */
int tdx_usb_claim(const char *path, int iface);
void tdx_usb_release(int fd, int iface);
/* Bulk OUT in 16 KiB URBs (same bus traffic as one transfer). 0 or -errno. */
int tdx_usb_bulk_out(int fd, unsigned ep, const uint8_t *buf, size_t len, unsigned timeout_ms);

#endif
