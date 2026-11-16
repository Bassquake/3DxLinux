# tdxd — 3Dconnexion driver for Linux (x86_64, arm64, …)

`tdxd` is a user-space Linux driver for 3Dconnexion devices. It focuses on the
**SpacePilot Pro** (046d:c629), including its 320×240 colour LCD.

It was rebuilt by reverse engineering the Windows package
`3DxWare64_v10-9-14_b745.exe` (see [docs/PROTOCOL.md](docs/PROTOCOL.md)).

The Windows driver can't simply be recompiled: it is a KMDF kernel filter plus
Win32 services. Instead, `tdxd` re-implements the device protocol those
components speak, using standard Linux interfaces:

| Windows component (3DxWare) | Linux replacement in `tdxd` |
|---|---|
| `3dxhid.sys` HID filter: report parsing, axis remap | `hidraw` reader + `report.c` / `process.c` |
| `3DxService.exe` + `Base.xml` device database | `devices_gen.c` (generated from `Base.xml`) + `/etc/tdxd.conf` |
| Input injection to apps | `uinput` virtual device and a spacenavd-compatible socket (`libspnav` apps) |
| Logitech `LCDMon` / `EDMBTDD.dll` / `LGPBTDD.sys` | `lcd.c` over usbfs: bulk frames and vendor brightness request |
| `HIDDev.dll` LED on/off | HID output report `{0x04, on}` |
| `HIDDev.dll` RGB LED colours (Giotto) | HID feature report `{0x1A, 0x25, led, R, G, B, 0, 0}` |
| `HIDDev.dll` LCD intensity (Enterprise) | HID feature report `{0x11, percent}` |
| `HIDDev.dll` `CWinUSBLCDMgr` (Giotto 1280×416 LCD) | `lcd_giotto.c`: rotated 24 bpp raw-deflate frames, double-buffered with 64-byte command packets (0x20/0x24/0x22); 0x21 rectangle writes; the legacy sp4LCD.bmp 0x13 path (optional directory watcher); slide animation between up to 4 pages and the vertical "move" operation, both with 0x23 rectangle copies |
| `HIDDev.dll` `CWinUSBLCDMgr` (Enterprise 640×150 LCD) | `lcd_ent.c` + `deflate.c`: 512-byte header packet and raw-deflate RGB565 frames over usbfs |
| `3DxLCDSPPRO.exe` "Function Keys" applet | built-in key-label pages: SpacePilot Pro keys 1–5 (SHIFT shows 6–10), Enterprise and Giotto keys 1–12; or any PPM image |

## Supported devices

There are 29 devices from the 3DxWare database plus 7 legacy PIDs (`tdxd -l`).
Device support is data-driven. Button layouts come from `Base.xml`, so other
3Dconnexion 3D mice work the same way. CadMouse and keyboards are skipped
because they already work through `hid-generic`.

## Build

The only build dependency is a C11 compiler with Linux UAPI headers. There are
no library dependencies.

```bash
make                 # native (x86_64 or arm64 host)
```

```bash
make ARCH=arm64      # cross-compile with aarch64-linux-gnu-gcc
```

```bash
make CROSS_COMPILE=riscv64-linux-gnu-   # any other GCC cross toolchain
```

```bash
make STATIC=1        # static binaries for appliances or Raspberry Pi images
```

```bash
make test            # host unit tests (report parsing, LCD framing, config)
```

The outputs are `build/<arch>/tdxd` (the daemon) and `build/<arch>/tdx-lcd`
(a standalone LCD tool).

## Install

```bash
sudo make install
```

```bash
sudo udevadm control --reload && sudo udevadm trigger
```

```bash
sudo systemctl enable --now tdxd
```

`tdxd` provides the spacenavd socket (`/run/spnav.sock`), so don't run
`spacenavd` at the same time (the service unit declares `Conflicts=`).
Edit `/etc/tdxd.conf` and apply the changes with `systemctl reload tdxd`.

### Prebuilt package (no compiler on the test machine)

`make dist` builds static, stripped binaries and packs them with the udev rule,
the service, the config and an install script. Use `make dist ARCH=arm64` for
arm64.

```bash
tar xzf tdxd-*-linux-x86_64.tar.gz && cd tdxd-*-linux-x86_64
```

```bash
sudo ./install.sh
```

```bash
sudo tdxd -v -v
```

Running the daemon in the foreground shows each detected device, every button
press (with `-v -v`), and any LCD / LED errors. `sudo ./install.sh --remove`
uninstalls everything except `/etc/tdxd.conf`.

### First hardware test checklist

1. `tdxd -l` lists the device's VID:PID, and `ls /dev/hidraw*` shows its node.
2. `sudo tdxd -v -v`, then move the cap and press buttons: events are logged.
   `evtest` shows the "3Dconnexion … (tdxd)" device.
3. LCD: stop the daemon, then run `sudo tdx-lcd fill bars`. On a SpacePilot Pro,
   also run `sudo tdx-lcd ping` and `sudo tdx-lcd brightness 50`.
4. If colours look wrong (red/blue swapped) or the image is upside down, note
   it. These are the inferred parts listed in `docs/PROTOCOL.md`.

The SpacePilot Pro LCD has only three brightness levels; requests snap to
30 %, 60 % or 90 %.

### Testing from Windows with WSL2

A Windows PC can run the Linux build against a real device by passing it into
WSL2 with [usbipd-win](https://github.com/dorssel/usbipd-win). That is how
[docs/HARDWARE_TESTS.md](docs/HARDWARE_TESTS.md) was done. While the device is
attached, Windows and 3DxWare cannot use it.

1. **Share the device.** In an admin PowerShell, run `usbipd list` and note the
   device's bus ID. Then run `usbipd bind --busid <id> --force`.
   - `--force` is needed while the 3Dconnexion / Logitech drivers own the device.
   - **Unplug and replug the device** so the forced bind takes effect.
2. **Load the modules.** In WSL, as root, keep a shell open so the VM stays
   running, then load the modules:

   ```bash
   modprobe vhci_hcd usbhid hid_generic uinput evdev
   ```

3. **Attach.** In PowerShell:

   ```bash
   usbipd attach --wsl --busid <id>
   ```

4. **Test in WSL.** Run `tdxd -v -v` and `tdx-lcd fill bars` as above.
5. **Give it back.** Run `usbipd detach --busid <id>`, then
   `usbipd unbind --busid <id>` as admin. Windows reloads its drivers.

## LCD

`tdx-lcd` finds the panel automatically: SpacePilot Pro (320×240), SpaceMouse
Enterprise (640×150) or Giotto (1280×416). Use `-d VID:PID` to pick one.

```bash
tdx-lcd preview page.ppm /etc/tdxd.conf   # render the key page offline
```

```bash
tdx-lcd fill bars                          # test pattern (stop tdxd or set lcd = no first)
```

```bash
tdx-lcd image splash.ppm                   # 320x240 binary PPM
```

```bash
tdx-lcd brightness 60
```

```bash
tdx-lcd ping                               # exit 0 if the panel answers vendor request 0x01
```

```bash
tdx-lcd slide p1.ppm p2.ppm p3.ppm         # Giotto: slide animation across 1280x416 pages
```

```bash
tdx-lcd move sheet.ppm background.ppm     # Giotto: push an image down, uncovering the background
```

```bash
tdx-lcd rect 0 0 1280 60 page.ppm         # Giotto: rewrite only that rectangle
```

```bash
tdx-lcd bmp sp4LCD.bmp                    # Giotto: legacy 0x13 path (24 bpp BMP)
```

## Status

| Feature | Evidence |
|---|---|
| Report formats, button masks, both LCD frame formats, brightness, ping, LED and RGB LED requests | Recovered from the shipped binaries (Ghidra decompilation) |
| Parsers, frame packers and LCD headers | Unit-tested |
| Deflate encoder | Round-trip checked against Python zlib in `make test` |
| x86_64 and arm64 builds | Compile without warnings |
| SpacePilot Pro on real hardware | **Verified** (2026-11-16, see [docs/HARDWARE_TESTS.md](docs/HARDWARE_TESTS.md)): motion, buttons, the uinput and spacenavd outputs, LCD frames, ping, brightness (3 levels), LED, and the daemon's key page with SHIFT paging |
| SpaceMouse Enterprise and Giotto | Not verified on hardware (no device available) |

`docs/PROTOCOL.md` lists the open items.

## Regenerating the device table

Extract `Base.xml` from `3DxWinCore64.msi`, then run:

```bash
make devices BASE_XML=/path/to/Base.xml
```
