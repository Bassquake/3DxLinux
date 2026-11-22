#!/usr/bin/env python3
"""Read-only probe of a 3Dconnexion Universal Receiver (256f:c652 and family).

    sudo python3 probe_receiver.py [seconds]

Prints every receiver interface's feature reports (0x41-0x47, 0x50, 0x60,
0x1a), then the reports that arrive for [seconds] (default 10). Sends nothing
to the receiver but HID GET_FEATURE requests."""
import array, fcntl, glob, os, select, sys, time


def ioc(d, nr, size):
    return (d << 30) | (size << 16) | (ord('H') << 8) | nr


def hexs(b):
    return " ".join("%02x" % x for x in b)


nodes = []
for n in sorted(glob.glob("/dev/hidraw*")):
    try:
        ue = open("/sys/class/hidraw/%s/device/uevent" % os.path.basename(n)).read()
    except OSError:
        continue
    if "0000256F:0000C6" not in ue.upper():
        continue
    phys = [l for l in ue.splitlines() if l.startswith("HID_PHYS=")]
    nodes.append(n)
    print("=====", n, ue.split("HID_ID=")[1].split()[0], phys[0][9:] if phys else "")
    fd = os.open(n, os.O_RDWR | os.O_NONBLOCK)
    size = array.array('i', [0])
    fcntl.ioctl(fd, ioc(2, 0x01, 4), size)
    print("descriptor %d bytes" % size[0])
    for rid in list(range(0x41, 0x48)) + [0x50, 0x60, 0x1a]:
        b = array.array('B', bytes([rid]) + bytes(63))
        try:
            r = fcntl.ioctl(fd, ioc(3, 0x07, 64), b)
            print("  feature 0x%02x: %s" % (rid, hexs(b.tobytes()[:r])))
        except OSError as e:
            print("  feature 0x%02x: %s" % (rid, e.strerror))
    os.close(fd)

if not nodes:
    sys.exit("no 3Dconnexion receiver found (is it plugged in? run with sudo)")
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10
print("===== reports for %.0f s" % secs)
fds = {os.open(n, os.O_RDONLY | os.O_NONBLOCK): n for n in nodes}
end, count = time.time() + secs, {}
while time.time() < end:
    r, _, _ = select.select(list(fds), [], [], 0.5)
    for fd in r:
        try:
            d = os.read(fd, 64)
        except OSError:
            continue
        k = (fds[fd], d[0])
        count[k] = count.get(k, 0) + 1
        if count[k] <= 4:
            print("%s %s" % (fds[fd], hexs(d)))
print("counts:", {"%s 0x%02x" % k: v for k, v in sorted(count.items())})
