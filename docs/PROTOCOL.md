# 3DxWare 10.9.14 (b745): reverse-engineering notes

These notes cover what was recovered from `assets/3DxWare64_v10-9-14_b745.exe` and
how the recovered behaviour maps onto `tdxd`. Each item is labelled:

- **Observed**: read directly from a shipped file (INF, XML, or Ghidra 12.1.4 decompilation).
- **Inferred**: concluded from observed code, not yet confirmed on real hardware.
- **Unknown**: not established.

## 1. What the installer contains

The installer is a 7-Zip SFX (LZMA2/BCJ). It holds 26 MSI packages plus redistributables.
Only these packages matter for hardware support:

| MSI | Relevant payload | Role on Windows |
|---|---|---|
| `3DxWinCore64.msi` | `3dxhid.sys`, `3dxhid.inf` | KMDF HID **filter** driver. It sits on the 3Dconnexion HID collections, merges and remaps reports, and injects input. |
| | `HIDDev.dll` | User-mode device I/O: HID feature reports, LEDs, the WinUSB LCD for newer devices. |
| | `3DxService.exe` | Policy and configuration. Loads `Base.xml` and per-app XML, then pushes the mappings down to the filter ("DownloadToKernel"). |
| | `Base.xml` (file key `_947C11BD…`) | **Data-driven device database**: VID/PID, class, and per-button report/mask/number. |
| `LADPInst.msi` | `LGPBTDD.sys`/`.inf`, `EDMBTDD.dll`, `LCDMon.exe` | Logitech LCD stack (the G19 one) that drives the **SpacePilot Pro LCD**. |
| `3DxLcdApplets64.msi` | `3DxLCDSPPRO.exe` | SpacePilot Pro "Function Keys" applet. It draws through the Logitech LCD Manager API. |

None of the hardware drivers can be "recompiled" for Linux: they are kernel-mode
Windows binaries (KMDF/WDM) and Win32 DLLs. The Linux port re-implements their
**device protocol** on top of standard Linux interfaces (hidraw, uinput, usbfs).

## 2. USB topology (observed: `3dxhid.inf`, `LGPBTDD.inf`)

```
SpacePilot Pro  VID 046D  PID C629
 ├─ interface 0  vendor-specific  -> LGPBTDD.sys  ("3Dconnexion SpacePilot PRO", G19 LCD class)
 └─ interface 1  HID              -> 3dxhid.sys   (HID\Vid_046D&Pid_C629&MI_01)
```

`LGPBTDD.inf` binds the same driver to `VID_046D&PID_C229&MI_00`, which is the
Logitech G19 LCD. The SpacePilot Pro LCD is therefore a G19-class 320×240 panel.

The INF also lists every supported PID. That list includes Bluetooth LE HID IDs
(`{00001812-…}_Dev_VID&02256f_PID&c638`) and Universal Receiver interfaces MI_00..MI_05.

## 3. HID input reports (observed: `3dxhid.sys`)

Recovered from the report parser `FUN_14000359c` and its inverse `FUN_1400037e4`:

| Report ID | Payload | Meaning |
|---|---|---|
| `0x01` | `int16le tx, ty, tz` | Translation. On devices whose `InputReportByteLength > 13` (from `HidP_GetCaps` in `FUN_140002be0`), the same report also carries `rx, ry, rz` at bytes 7..12. |
| `0x02` | `int16le rx, ry, rz` | Rotation. |
| `0x03` | 1–4 bytes, little-endian bitmask | Buttons (SpacePilot Pro: 31 bits). |
| `0x1C` | 6 × `uint16le` | List of currently pressed **button numbers** (Enterprise, Giotto). |
| `0x1D` | `uint32le` | Long-press bitmask (Enterprise, Giotto). |

`FUN_140002ea0` then applies a per-application transform: sign/inversion per axis,
a dominant-axis filter, and merging of reports 1 and 2. The transform's
parameters come from `3DxService` through custom IOCTLs:

- `0x22A008`: 4-byte mode.
- `0x22A010`: 0xA4-byte mapping block.

`tdxd` re-implements the same options in user space (`sensitivity`, `scale_*`,
`invert_*`, `map_*`, `dominant`, `disable_*`).

### 3.1 Calibration / re-zero (observed: `HIDDev.dll`, `3DxService.exe`)

The control panel's **Calibrate** button reaches the same function as the
assignable action `Driver_RezeroDevice`. `3DxService.exe` resolves the export
`UdDevRezero` from `HIDDev.dll` with `GetProcAddress` and calls it.
`UdDevRezero` (`0x180026f60`) logs "UdDev: Rezeroing the device" and queues a
2-byte **feature report** for `HidD_SetFeature`:

```
{ 0x07, 0x00 }
```

The report is only sent when the HID `VersionNumber` passes a check. That field
is at offset `0x58`, after `HidD_GetAttributes` filled a `HIDD_ATTRIBUTES` at
`+0x50`, and it is the USB `bcdDevice`:

- `VersionNumber >= 0x200`, and
- for the SpaceMouse Enterprise (C633), `VersionNumber > 0x441`.

The SpacePilot Pro's HID descriptor declares feature report `0x07` with one data
byte (see `docs/HARDWARE_TESTS.md`).

`3DxService.exe` and `HIDDev.dll` also read `AutoCalibrateEnabled`,
`AutoCalibratePeriod` and `AutoCalibrateWindow` settings.
- **Unknown:** what auto-calibration does. It was not traced, and `tdxd` does
  not implement it.

**What `tdxd` does:**

- It reads `bcdDevice` from sysfs and sends `{0x07, 0x00}` with
  `HIDIOCSFEATURE` under the same version check.
- When the version is unknown, for example over Bluetooth, it sends the report
  anyway.
- Triggers: `tdxctl calibrate`, `calibrate_button = V3DK_…`, or
  `calibrate_on_start = yes`.

### 3.2 Per-application configuration

3DxWare keeps one XML configuration per application, for example `Blender.xml`
or `SolidWorks.xml` in `3DxWinCore64.msi`, and switches with the focused window.

`tdxd` implements this as overlay files in `/etc/tdxd.d/*.conf`, chosen by
process name. The focused application comes from `tdx-focus` (X11
`_NET_ACTIVE_WINDOW` → `_NET_WM_PID` → `/proc/PID/exe`, or sway's IPC), or else
from the newest spacenavd client (`SO_PEERCRED`). The 3DxWare XML files are not
imported. Their actions refer to Windows applications' commands.

## 4. Button layout (observed: `Base.xml`)

Each `<Device>` entry lists its buttons as one of:

- `<Event><ReportID>03</ReportID><Mask>00000004</Mask></Event>`
- `<Event><ReportID>1C</ReportID><Number>3</Number></Event>`
- `<Event><ReportID>1D</ReportID><Number>3</Number><LongPressMask>00000004</LongPressMask></Event>`

Each button also has a `V3DKID` (for example `V3DK_TOP`), which is the stable
virtual-key identity used by 3DxWare.

`tools/extract_devices.py` converts all 33 `<Device>` entries into
`src/devices_gen.c`. This covers 29 real devices; parents and the pseudo-device
are skipped. It also adds 7 legacy PIDs that this release no longer lists, with
buttons numbered generically.

SpacePilot Pro (report 0x03):

| bit | V3DK | bit | V3DK | bit | V3DK |
|---|---|---|---|---|---|
| 0 | MENU | 11 | ISO2 | 22 | ESC |
| 1 | FIT | 12–21 | 1 … 10 | 23 | ALT |
| 2 | TOP | | | 24 | SHIFT |
| 3 | LEFT | | | 25 | CTRL |
| 4 | RIGHT | | | 26 | ROTATE |
| 5 | FRONT | | | 27 | PANZOOM |
| 6 | BOTTOM | | | 28 | DOMINANT |
| 7 | BACK | | | 29 | PLUS |
| 8 | ROLL_CW | | | 30 | MINUS |
| 9 | ROLL_CCW | | | | |
| 10 | ISO1 | | | | |

## 5. SpacePilot Pro LCD (observed: `EDMBTDD.dll`, `LGPBTDD.sys`)

### Frame transfer

`FUN_180001980` calls `WriteFileEx(h, buf, 0x25A00)`. `LGPBTDD.sys` turns that
write into a bulk OUT transfer on interface 0. The buffer is 154 112 bytes:
a 512-byte header followed by 320×240 RGB565 pixels.

Header, built in `FUN_1800015a0`:

```
off  0: 10                 opcode
off  1: 0F
off  2: 00 58 02 00        u32le 0x25800 = pixel byte count
off  6: 00
off  7: 00 00  00 00       x0 = 0, y0 = 0
off 11: 3F 01  EF 00       x1 = 319, y1 = 239
off 15..511: byte value == its own offset (0x0F, 0x10, … 0xFF, 0x00, … 0xFF)
```

Pixels, converted in `FUN_180001c40`:

- Input is 32 bpp `0x00RRGGBB`, row-major.
- Output is `uint16le RGB565 = (R>>3)<<11 | (G>>2)<<5 | (B>>3)`.
- Output is **column-major**: `dst[x*240 + y] = src[y*320 + x]`.

### Brightness

`LCDExtDeviceManagerSettingsUpdatedByUser` issues IOCTL `0x220014` with the
8-byte block `{0x0A, 0, wValue=0, wIndex=0, wLength=9}` followed by 9 data bytes.
`data[0] = (brightness + 1) & 0x7F`.

`LGPBTDD.sys` `FUN_00011edc` forwards the block as `URB_FUNCTION_VENDOR_INTERFACE`
(0x18). On the wire this is:

```
bmRequestType 0x41  bRequest 0x0A  wValue 0  wIndex 0 (interface)  wLength 9
```

At initialisation the current value is read back with the same request in the
IN direction (`0xC1`, IOCTL `0x220010`) and decremented by one.
`bRequest 0x01` (`0xC1`, 9 bytes) is a presence/ready ping. On resume,
`LCDExtDeviceManagerResumeReEnumerate` calls `FUN_180002530`, which sends it to
every LCD up to 100 times, 100 ms apart, until it succeeds. It then sleeps
500 ms before re-enumerating.

`tdxd` does the same through `tdx_lcd_wait_ready()`, with at most 20 tries
(2 s):

- after opening the LCD;
- again before retrying a failed frame write.

`tdx-lcd ping` sends a single ping.

- **Inferred:** the brightness range is 0..100. The Windows default is 80 for the
  "Cartman" C250 prototype. The SpacePilot Pro's own default is read from the device.
- **Observed on hardware** (2026-11-16, `docs/HARDWARE_TESTS.md`): the SpacePilot
  Pro has **three** backlight levels. Writes snap to raw 30, 60 or 90, which read
  back as 29, 59 or 89:

  | Value set | Raw level |
  |---|---|
  | 0–30 | 30 |
  | 40–60 | 60 |
  | 75–100 | 90 |

  The panel's starting value was 89. The exact thresholds in 31–39 and 61–74
  were not measured.
- **Unknown:** the meaning of the other 8 data bytes. They are uninitialised stack on
  Windows, and `tdxd` sends zeros, which the device accepted.

**Hardware status** (SpacePilot Pro, 2026-11-16):

- The frame format (header, RGB565, column-major) produced a correct image,
  with colours and orientation right.
- The ping answers.
- Brightness works within the three levels above.
- The bulk OUT endpoint is `0x02` (512-byte packets).
- Interface 0 also has an interrupt IN endpoint `0x81` (2 bytes). It is not
  used by `tdxd`; it is probably the LCD soft-button channel of the G19 family.

## 6. LEDs (observed: `HIDDev.dll`)

`FUN_180033ab0` (`CUSBDev` LED on/off) writes a 2-byte HID **output report**
with `WriteFile` on the HID handle:

```
{ 0x04, state }     state: 1 = on, 0 = off
                    (3 instead of 1 on the Enterprise C633 when a device flag is clear)
```

Giotto (C63C, `-0x39c4`) branches to `FUN_180033cd0` instead. See 6.1.

`tdxd` sends `{0x04, led}` when `led = yes|no` is set (all devices except Giotto).

**Observed on hardware** (SpacePilot Pro, 2026-11-16):

- The HID report descriptor declares report `0x04` as a 1-byte output on the
  LED usage page.
- Writing `{0x04, 0}` and `{0x04, 1}` to hidraw visibly switches a light on the
  device off and on.

### 6.1 Giotto RGB LEDs (observed: `HIDDev.dll`)

`FUN_180034af0` (logged as `CUSBDev::SetLEDColor`) builds an 8-byte buffer and
queues it with `FUN_180033360`. The device write thread sends each queued buffer
with `HidD_SetFeature(handle, buf, len)`. So this is a **feature report**:

```
{ 0x1A, 0x25, index, R, G, B, 0x00, 0x00 }
```

LED indices come from the device setup in `FUN_18002b9f0`:

| index | LED |
|---|---|
| `0x00`–`0x1D` | 30 button LEDs. Index = `<LEDNumber>` in `Base.xml` (Giotto buttons use 0–29). |
| `0x1E`–`0x23` | 6 ring segments, at table offsets `+0xf44..+0xf58`. |
| `0x24` | Sent with black by `FUN_180033cd0` when the LEDs are switched **off**. |
| `0x25` | Sent with one colour by `FUN_180033cd0` when all 30 button LEDs share it (broadcast). |
| `0x26` | Sent by `FUN_180032e70` case 10 (`RingLEDColor`) with the ring colour (broadcast). |

How `FUN_180033cd0` applies the table:

- **Off:** it sends `0x24` with `(0,0,0)` and clears the table.
- **On, all 30 button colours equal:** it sends one `0x25` report.
- **On, button colours differ:** it sends one report per button LED, then one per ring segment.

`FUN_180032e70` case 14 updates a single LED from a packed
`idx | R<<8 | G<<16 | B<<24` and marks the table dirty; case 15 commits it.

`tdxd` reproduces this in `tdx_rgb_plan()` and writes the reports with
`HIDIOCSFEATURE(8)`.

| Setting | Effect |
|---|---|
| `led = no` | Sends the `0x24` "off" report. |
| `led = yes` | Without other colour settings, sets buttons and ring to white. |
| `led_button_color` | Colour for all button LEDs. |
| `led_ring_color` | Colour for the ring. |
| `led_color_<V3DK id \| LED_n>` | Colour for a single LED. |

- **Inferred:** the meanings of the broadcast indices `0x24`/`0x25`/`0x26` come
  from how each is used, not from firmware.

### 6.2 SpaceMouse Enterprise LCD intensity (observed: `HIDDev.dll`, `3DxService.exe`)

The Enterprise (C633) sets its LCD brightness with a 2-byte HID **feature report**:

```
{ 0x11, intensity }      intensity: 0..100 (percent)
```

Evidence:

- `HIDDev.dll` `FUN_180032e70` (dispatcher for the exported `UdDevSetDisplay`),
  case 8, stores the value at `+0xebc` and queues `{0x11, value}`. The queue is
  sent with `HidD_SetFeature`.
- `FUN_18002b9f0` resends the stored value whenever an Enterprise is opened. It
  then sleeps 3 s and resets the WinUSB LCD manager.
- In `3DxService.exe`, `FUN_1400af6a0` case 8 writes the same field to `+0x380`.
  `FUN_1400adf40` saves that field as `<LCDIntensity>%d</LCDIntensity>` in `LCDCfg.xml`.
- The 0..100 range comes from the `Driver_LCD*` actions:
  - `Driver_LCDOn` sets the intensity to 100, or to the value saved before it was turned off.
  - `Driver_LCDOff` saves the current value and sets it to 0.

`tdxd` sends `{0x11, lcd_brightness}` to an Enterprise on attach and on reload,
when `lcd_brightness` is set. This is the same option that controls the
SpacePilot Pro's backlight, which uses its own USB vendor request (section 5).

- **Unknown:** whether the 3 s settle is needed. `tdxd` does not wait, because it
  does not drive the Enterprise's WinUSB LCD.

For the SpacePilot Pro, `HIDDev.dll` `FUN_180034060` also notifies the Logitech LCD
Monitor window (`PostMessage(WM_CLOSE)`) on device changes. This confirms that the
LCD is owned by the Logitech stack and not by `HIDDev`.

### 6.3 SpaceMouse Enterprise LCD image (observed: `HIDDev.dll` `CWinUSBLCDMgr`)

The Enterprise has a 640×150 colour LCD. It sits on its own USB interface: 0 for
the LCD, while HID is interface 1 (`MI_01`, the same split as the SpacePilot Pro).
On Windows that interface is opened through WinUSB. `FUN_180048480` gives the
packet size used for command packets: `0x200` for C633 and `0x40` for C63C.

**Image source.** `3DxService` renders each LCD page to a little-endian TIFF.
`FUN_18004c440` (`WinUSBLCD:SendTIFFFileToDevice`) parses the TIFF IFDs and
concatenates the strips into a `0x46500`-byte buffer, which is 640×150×3. A
multi-page TIFF becomes an animation, played back with `QueryPerformanceCounter`
timing.

**Frame encoding** (`FUN_18004bbd0`, `WinUSBLCD:SendImage`):

1. Each 3-byte pixel `b0 b1 b2` becomes RGB565:
   `(b2 & 0xF8) << 8 | (b1 & 0xFC) << 3 | b0 >> 3`. The output is row-major,
   640 per row, `0x2EE00` = 192 000 bytes in total.
2. Any resulting `0x0000` is replaced by `0x0821`, so that 0 can mean
   "unchanged".
3. For animation frames after the first, every pixel equal to the previous frame
   is set to 0. This is a "mask" (delta) frame. The full frame is kept as the
   next reference.
4. The 192 000 bytes are compressed with miniz `tdefl`. `FUN_180046980` is
   `tdefl_init`, with flags `0x46080`:

   | Flag | Meaning |
   |---|---|
   | `0x40000` | Force static-Huffman blocks |
   | `0x4000` | Greedy parsing |
   | `0x2000` | Compute Adler-32 |
   | `0x80` | 128 probes |

   `WRITE_ZLIB_HEADER` (0x1000) is **not** set; it is tested at the
   `& 0x1000` site in the compressor. The result is therefore a **raw deflate**
   stream. The output buffer is `0x33980` bytes.
5. One header packet is sent, zero-padded to the packet size (512 bytes), followed by
   the compressed bytes. Both go to the same bulk OUT pipe.

```
off 0     opcode   0x11 full frame ("LCD_Send_Header_comp")
                   0x18 delta frame ("LCD_Send_Header_comp_mask")
                   0x15 first frame after open ("LCD_Send_Header_comp_slide")
off 1     0x0F
off 2..5  compressed length, u32 LE
off 6..13 00 00 00 00  00 3F 01 EF      (0x18: all zero)
off 14..15 00 0F
```

This is the same layout as the SpacePilot Pro/G19 header in section 5, with a
"compressed" opcode in place of `0x10`. In both, the rectangle fields hold the
constant 319×239.

6. After the first frame following open (`0x15`), Windows logs "waiting for LCD
   to init" and sleeps 5 s.

**What `tdxd` does** (`src/lcd_ent.c`, `src/deflate.c`):

- It sends the same header and a raw-deflate stream. The stream is a single
  fixed-Huffman block with greedy hash-chain LZ77, which is the block type
  Windows forces. Worst case is 202 389 bytes for random data, within the
  Windows buffer.
- Instead of blocking for 5 s, `tdxd` marks the panel busy and redraws from its
  main loop once the 5 s are up.
- Page updates are sent as delta frames (`0x18`).
- `tdx-lcd` sends a single `0x15` frame and exits.

**Inferred channel order.** The Windows code puts byte 2 in the top 5 bits.
Whether that byte is red depends on how 3DxService writes the TIFF. Windows
bitmaps are BGR in memory, which would make it standard RGB565. `tdxd` assumes
standard RGB565 (red in the top bits); `lcd_swap_rb = yes` flips it.

### 6.4 Giotto LCD (observed: `HIDDev.dll` `CWinUSBLCDMgr`)

Giotto (256f:c63c) has a 1280×416 colour LCD on its WinUSB function. `tdxd` claims
the first vendor-class interface (class `0xFF`) that has a bulk OUT endpoint,
rather than assuming interface 0.

**Panel descriptor.** This is static data at `DAT_1800de3e0`, read from the PE
`.data` section:

```
{ pid 0xC63C, rotate 1, width 1280, height 416, bytes/pixel 3, height-1 415, -1, 1 }
```

The neighbouring Enterprise descriptor is `{640, 150, 2, 149}`.

**Pixels** (`FUN_18004a760`, rotate mode). Each pixel is copied from a 32 bpp
source row as its first three memory bytes:

```
out[((w-1-x) * h + (y - y0)) * 3 + k] = src(x, y)[k]      k = 0..2
```

The panel is therefore fed 1280 reversed columns of 416 pixels each, 3 bytes per
pixel, for 1 597 440 bytes per full frame. Compression is the same miniz `tdefl`
raw deflate as the Enterprise, into a `0x1ad080`-byte output buffer.

**Command packets** (`FUN_180048530` … `FUN_180048980`). Each is 24 meaningful
bytes, with `55 AA` at offsets 22–23, zero-padded to the packet size. That size
is 64 bytes for C63C (`FUN_180048480`). Multi-byte fields are little-endian.

| op | bytes | meaning (inferred from use) |
|---|---|---|
| `0x20` | `20 00 buf 01 len32` | write a full compressed frame into device buffer `buf`; payload follows |
| `0x21` | `21 00 buf 01 len32 00×4 y16 mx16 h16 w16` | write a rectangle (`mx = 1280 - w - x`, mirrored by the rotation) |
| `0x22` | `22 b1 b2` | copy whole buffer `b1` → `b2` |
| `0x23` | `23 src dst 00×5 sy16 smx16 dy16 dmx16 h16 w16` | copy a rectangle from `src` to `dst` (see 6.5) |
| `0x24` | `24 buf` | show buffer `buf` |

**Buffers and the static-image sequence:**

- `FUN_180048fb0` starts with A = 1 (back), B = 0 (front), C = 3 and D = 2,
  and sends `24 {B}`.
- `CWinUSBLCDMgr::SetLCDImage` (`FUN_180049e10`) with usage 0 (static image)
  calls `FUN_18004b2e0` (`SendImageToDevice_RGB888`), which calls
  `FUN_18004aee0` with buffer A:

```
20 {A, len}  + compressed payload
swap A <-> B
24 {B}                 show the frame just written
22 {B, A}              copy it into the new back buffer
22 {B, C}              and into C
```

The slide animation (`FUN_180049fd0`) is described in 6.5.

**Legacy path.** `FUN_18004a8a0` (`SendBMPFileToDevice_RGB888`) watches the file
`sp4LCD.bmp` and sends it with a G19-style `0x13` header (see 6.8).

**What `tdxd` does** (`src/lcd_giotto.c`):

- On attach it sends `24 {0}`, then each redraw follows the static-image
  sequence above.
- It refuses frames that would not fit Windows' output buffer (`-EFBIG`). A
  rendered key page compresses to about 18 KB.

**Inferred source layout.** Two things depend on the source buffer the service
renders, which is not visible in `HIDDev.dll`:

- **Channel order:** on Windows a 32 bpp DIB is `B,G,R,X` in memory, so the
  panel gets `B,G,R`. `lcd_swap_rb` flips this.
- **Row order:** the source rows are assumed to be top-down. `HIDDev`'s own BMP
  loader flips bottom-up DIBs to top-down before using this converter.

### 6.5 Giotto slide animation (observed: `HIDDev.dll` `FUN_180049fd0`)

`UdDevSetDisplay` case 0xd ("LCDImageOp") reaches `FUN_180049fd0`, where
operation type 1 is `SiImageOpType_Slide`.

**Pages.** `SetLCDImage` (`FUN_180049e10`) caches up to four 1280×416 slide
pages in slots (0..3, 1). Their descriptors sit at `+0x2ee80`, `+0x2ef28`,
`+0x2efd0` and `+0x2f078`; slot k = page × 3 + 1, each slot 0x38 bytes. "Moving"
images (usage 2) are cached only; the device is not touched until a slide
starts.

**Position.** The slide position `x` lies in `[0, 0x1400)`; anything outside is
logged as "unexpected x value". The page is `x / 1280` and the offset is
`x % 1280`.

**Device buffers.**

- Buffer D (`+0x2f118`, initially 2) holds page p.
- Buffer C (`+0x2f114`, initially 3) holds page p+1.
- Each is written with `0x20` (no flip) only when the cached page has changed.
  `+0x2f0ec/+0x2f0f0/+0x2f0f4` and `+0x2f0f8/+0x2f0fc/+0x2f100` track the cached
  slot and its dirty flag.

**Each step.** Two rectangle copies compose the slide band, rows 60..400
(`y = 0x3C`, `h = 0x155`), into back buffer A. Then A and B swap, `24 {B}`
shows the result, and Windows sleeps 25 ms:

```
23 {src D, dst A, sy 60, smx 0,        dy 60, dmx off, h 341, w 1280-off}
23 {src C, dst A, sy 60, smx 1280-off, dy 60, dmx 0,   h 341, w off}      (if off > 0)
swap A <-> B ; 24 {B}
```

**Last page.** For `x` in `[3840, 5120)`, page 3 is copied whole from C:
`23 {C, A, 60, 0, 60, 0, 341, 1280}`.

**0x23 layout.** Bytes `[1]` = source, `[2]` = destination, then from byte 8 the
u16 fields source y, source mx, destination y, destination mx, h, w.

- **Inferred:** the field roles. "mx" is the mirrored column, `1280 - w - x`,
  matching the rotated pixel layout.
  - The slide calls do not settle which x field is the source and which is the
    destination. Swapping them also gives a coherent slide, in the opposite
    direction, with the next page entering from the left.
  - `tdxd` uses the conventional direction: as `x` increases the content moves
    left and the next page enters from the right.
  - Both slide calls use equal source and destination y (60), so they cannot
    distinguish the y fields either. The vertical move (6.6) uses different
    values; with this reading it moves its image down.
  - A device model in `tests/test_core.c` executes the planned packets under
    this reading. It confirms the on-screen result at offsets 1, 320, 1279,
    1280, 1900 and the last page.

**What `tdxd` does** (`tdx_gio_plan_slide` / `tdx_giolcd_slide`):

- It follows the same sequence. Pages are uploaded only when they change. C is
  not uploaded at offset 0, because it isn't read there.
- The renderer keeps the title bar in rows 0..59 and the key grid inside the
  slide band, so a slide moves the whole grid.
- With `lcd_label_13`..`lcd_label_48` set, the daemon builds 2–4 pages.
  `lcd_slide_button = V3DK_…` slides to the next page at `lcd_slide_step`
  pixels per 25 ms. When the slide ends, the daemon:
  1. syncs the back buffer (`22 {B, A}`), because the slide swaps buffers
     without syncing them;
  2. rewrites only the 1280×60 title bar with a `0x21` rectangle write (6.7),
     so the page dots update.
- `tdx-lcd slide P1.ppm P2.ppm [P3 P4]` animates arbitrary pages.

Operation type 2 only logs; any other type logs "Unsupported image operation
type".

### 6.6 Giotto vertical move (observed: `HIDDev.dll` `FUN_180049fd0`, type 0)

Operation type 0 logs "LCDImageOp: move _Moving to x: %d, y: %d". It moves the
"moving" image vertically over a background.

**Images.**

- The moving image is the usage-2 image in slot (0,1). `SetLCDImage` only caches
  it ("Not waking thread to send to FW until swiping begins").
- `FUN_18004b2e0` writes slot (0,1) into buffer D and slot (2,1) into buffer C,
  with `0x20` and no flip.

**Each call to (x, y):**

1. Negative `y` is ignored; the gate is `-y + 0x1a0 < 0x1a1`.
2. If `y < 356` (`0x164`), the moving image's rows from 60 are drawn at row
   `y + 60`, clipped to the panel:

   ```
   23 {src D, dst A, sy 60, smx 0, dy y+60, dmx 0, h 356-y, w 1280}
   ```

3. If `y` grew since the previous call (`prev` at `+0x2f108`, initially 0), the
   newly uncovered rows are restored from the background:

   ```
   23 {src C, dst A, sy prev, smx 0, dy prev, dmx 0, h y-prev, w 1280}
   ```

   If either value exceeds 416 (`0x1a0`), Windows returns here without recording
   the new position.
4. The position is stored at `+0x2f104/+0x2f108`.

**No show.** Windows sends no `0x24` in this operation, so the result stays in
back buffer A until something else presents it.

**Restored rows.** The restored band `[prev, y)` is not offset by 60, while the
image is drawn from `y + 60`. During the move, the 60 rows in `[y, y+60)` keep
whatever buffer A held before. After a move to 416 the whole buffer is
background. `tdxd` reproduces this as-is.

**What `tdxd` does** (`tdx_gio_plan_move` / `tdx_giolcd_move`):

- **Windows-exact steps:** the planner emits exactly the steps above, including
  the edge cases, which are covered by tests.
- **Present (`tdxd` addition):** since Windows never shows the result,
  `tdx_giolcd_move(…, present = 1)` adds `24 {A}` after each step.
  `tdx_giolcd_move_finish()` then makes A the front buffer and copies it to the
  new back buffer (`22 {B, A}`).
- **Device model:** the model in `tests/test_core.c` checks that, when
  presented, rows above `y` show the background and rows from `y + 60` show the
  moving image shifted down by `y`.
- **CLI:** `tdx-lcd move [-s PX] [-w] MOVING.ppm BACKGROUND.ppm` shows MOVING,
  then pushes it down off the panel, uncovering BACKGROUND. `-w` sends only the
  Windows commands, with no present.
- **Daemon:** `tdxd` has no event that triggers this operation; it is
  available through the library and `tdx-lcd`.

### 6.7 Giotto rectangle write (observed: `HIDDev.dll` `FUN_18004aee0`, `FUN_180048630`)

`FUN_18004aee0` writes any rectangle `(x, y, w, h)` that is not the full
1280×416 panel through the `0x21` path:

1. **Pixels.** `FUN_18004a760` copies the rectangle in its own rotated,
   rectangle-local order:
   `out[((w-1-i) * h + j) * 3 + k] = pixel(x+i, y+j)[k]`, with bytes B,G,R.
   The data is then raw-deflated. The output buffer is
   `max(n + n/0x7c00*5 + 0x85, w*h*330/100 + 0x80)` with `n = w*h*3`. For the
   full panel this equals the 0x20 buffer, `0x1ad080`.
2. **Commands**, with `mx = 1280 - w - x` (mirrored column):

   ```
   21 00 {A} 01 len32 00 00 00 00 y16 mx16 h16 w16 00 00 55 AA   + compressed payload
   swap A <-> B
   24 {B}                                              show
   23 {src B, dst A, sy y, smx mx, dy y, dmx mx, h, w} bring the rectangle into the new back buffer
   22 {B, C}                                           refresh C
   ```

   The conditions come straight from the code:
   - The swap and `24` happen only when the target buffer is A and the image's
     usage is 0 (static).
   - The `23` sync runs when the target is then the front buffer B.
   - The `22 {B, C}` copy runs only for usage 0.
   
   A write to C or D (slide pages) is therefore just `21`/`20` plus the payload.

**Back-buffer sync.** Only the rectangle is synced into the back buffer, so the
rest of A must already match B. That holds after static frames, which copy
`22 {B, A}`. It does not hold after a slide (6.5), whose steps swap without
syncing. `tdx_giolcd_sync_back()` sends `22 {B, A}` to restore it.

**What `tdxd` does** (`tdx_gio_plan_rect` / `tdx_giolcd_show_rect`):

- It follows this sequence. A full-panel rectangle falls back to the `0x20`
  path, as on Windows.
- It refuses payloads larger than the Windows buffer.
- **Device model:** the model in `tests/test_core.c` decodes the real packed
  `0x21` payload into a simulated buffer. It checks that the rectangle appears at
  the right on-screen position, that nothing outside it changes, and that the
  back buffer and C end up in sync, including after a slide.
- **Daemon:** `tdxd` uses it to refresh the title bar after a slide.
- **CLI:** `tdx-lcd rect X Y W H IMAGE.ppm` rewrites any rectangle from a
  full-size image.

### 6.8 Giotto legacy BMP path (observed: `HIDDev.dll` `FUN_18004a8a0`, `FUN_180049c00`)

**Trigger.**

- `WinUSBLCDMgtThread` (`FUN_180048a80`) waits on `FindFirstChangeNotification`
  for the directory `%LOCALAPPDATA%\3Dconnexion\3DxWare\Cfg`. That path comes
  from `SHGetFolderPathW(0x801C)` followed by `PathAppend`.
- On each change, `FUN_180049c00` compares the file's write time with the last
  one it saw. For a Giotto (manager mode 2) it calls
  `SendBMPFileToDevice_RGB888` on `sp4LCD.bmp`, then **deletes the file**.
- The Enterprise (mode 1) uses the same mechanism for its TIFF file (6.3).

**Rectangle.** The default is the full panel: x = 0, y = 0, w = 1280, h = 416.
If `sp4LCDControl.txt` exists in the same directory, it is read with
`fwscanf(L"(%d, %d), (%d, %d)", &x, &y, &w, &h)`.

- Four pointers are passed; Ghidra shows only two, but the call site's
  disassembly passes `r8`, `r9` and two stack arguments.
- Fields that do not match keep their defaults.

**Conversion** (`FUN_18004a620`):

1. The BMP is loaded with `LoadImageW(LR_LOADFROMFILE | LR_CREATEDIBSECTION)`
   and inspected with `GetObjectW`. It must have `planes × bpp > 8`.
2. The output buffer has `(bpp/8) × w × h` bytes, zero-filled.
3. Rows `y .. min(y+h, bmHeight)` are read as bottom-up DIB memory: row `r`
   starts at `(bmHeight − 1 − r) × bmWidthBytes`.
4. Columns `x .. min(x+w, 1280)` are copied 3 bytes each into the rectangle-local
   rotated layout used by `0x21` (6.7).

Two quirks follow from step 4:

- The source step is always 3 bytes, so only 24 bpp BMPs convert correctly.
- For 32 bpp, the compressed buffer still holds `4 × w × h` bytes, a zero tail
  included.

A top-down BMP (negative height) stays top-down in a DIB section, so it is read
upside down.

**Transfer.**

1. The buffer is compressed with the same raw deflate. The output limit is
   `max(n + n/0x7c00×5 + 0x85, n×110/100 + 0x80)`.
2. A 64-byte packet carries the header:

   ```
   13 0F len32 00 y16 mx16 h16 w16 0F      (mx = 1280 - w - x)
   ```

   This is the G19 / SpacePilot Pro header (section 5) with the rectangle in
   rotated coordinates.
3. The payload follows on the same pipe.

No buffer number and no `0x24` are involved. The legacy path writes the panel
directly, without the A/B/C/D buffer protocol.

**What `tdxd` does:**

- **Library:** `tdx_bmp_load`, `tdx_gio_bmp_control`, `tdx_gio_bmp_convert`
  and `tdx_giolcd_send_bmp` reproduce this, including both quirks.
- **One safety change:** where Windows would read past a short BMP row,
  `tdxd` stops copying that row instead.
- **Tests:** for a 24 bpp bottom-up BMP, the converter's output is
  byte-identical to the `0x21` and `0x20` packers.
- **CLI:** `tdx-lcd bmp FILE.bmp [CONTROL]` sends a file through this path.
- **Daemon:** with `lcd_bmp_dir = DIR`, `tdxd` emulates the watcher with
  inotify (`IN_CLOSE_WRITE` / `IN_MOVED_TO`). When `DIR/sp4LCD.bmp` appears it
  sends the file to every attached Giotto, then deletes it. A file that is
  already there when a Giotto attaches is sent too. With no Giotto attached the
  file is left alone.

## 7. Universal Receiver (observed: `HIDDev.dll`, a 256f:c652 receiver)

The receiver has five HID interfaces. Each one has the same vendor collection
(usage page 0xFF0A) with feature reports 0x41 (4 data bytes), 0x42–0x47,
0x50, 0x60 and 0x1A (7 data bytes each). Interface N carries the device in
pairing slot N; while a device is paired, its interface's descriptor also has
that device's collections.

- **Pairing table:** `HIDDev.dll`'s GetPairingTable reads feature `0x43 + slot`.
  Data bytes 0–1 are a little-endian device code (0 = empty slot); the other
  five bytes are the paired device's id. 3DxWare's serial number is the
  code's letters and the id in decimal: `59 00 | 00 48 f7 ba 8b` is
  Y1224194699 (its log and Advanced Settings show it). Feature 0x42 on an interface returns
  the same entry for that interface's own slot.
- **Device codes** (table at 0x1800dbca0: code, product id, class 1 = 3D mouse,
  2 = 2D mouse, 4 = receiver, 5 = keyboard): 'D' c631, 'A' c62e, 'T' c651,
  'U' c654, 'V' c657, 'Y' c658, 'N' c665, 'Q' c638, 'Z' c63a, 'KA' c664,
  'UC' c669, 'KC' c665, 'UR' c652.
- **Battery:** input report 0x17. Data byte 0 is the level in percent
  (0 = unknown), byte 1 bit 0 means charging. A connected CadMouse sends it
  about every 1.5 s while it is in use. Windows also asks for it with
  `HidD_GetInputReport`; on Linux `HIDIOCGINPUT` for 0x17 fails (EPIPE), so
  tdxd learns the level only once the mouse sends it.
- **Connected or not:** the receiver does not say. Its pairing table, feature
  0x50 (`50 00 00 00 00 59 00 10`: the slot codes again) and the others stay
  the same while the CadMouse is switched off, and no report arrives. 3DxWare
  (`HIDDev.dll`'s ActiveState) marks a device active on its first input report
  and never times it out; only the same device turning up active on another
  connection makes it inactive. `tdxd` does the same (`active yes|no`).
- **CadMouse input** (report 0x1B on its interface): buttons (8 bits), X and Y
  (int16), wheel (int16), tilt (int8). `hid-generic` turns it into a normal
  mouse, so `tdxd` only reads along.

- **CadMouse configuration:** feature report 0x10 (31 data bytes) on the
  mouse's interface. `HIDDev.dll`'s SBHID_SendPIDConfig_2DMouse builds it and
  sends it over the `\\.\pipe\TDXBROKERAPI_PIPENAME` pipe (command 6, "SetBlob")
  to `Mgl3DCtlrRPCService.exe`. That service passes it on with
  `IOCTL_HID_SET_FEATURE`. The mouse applies it itself. It can't be read back
  (GET_FEATURE fails with EPIPE). Data bytes:

  | Byte | Meaning |
  |---|---|
  | 1 | DPI / 50 (3DxWare's Speed slider: 200 … 1400 (default) … 7200) |
  | 2 | lift-detection threshold; 0x1f = lift detection off (3DxWare: 0) |
  | 3–6 | wheel mode: `01 ff 00 00` normal, `00 00 00 01` Smart Scrolling ("inertial wheel emulation") |
  | 18–24 | button map for buttons 1–7: `0a 0b 0c 0c 0e 0d 2f` (Swap Middle / Right: `0a 0c 0b …`) |
  | 26 | 0x1e |
  | 30 | update-rate divider: 1, 2, 4, 8 = 1000, 500, 250, 125 Hz (Polling Rate) |

  Every other byte is 0. 3DxWare resends the whole block on every change.

  The button map: **9 + n** makes a button mouse button n (1 left, 2 right,
  3 middle, 4 back, 5 forward); the mouse does it itself ("sent to system
  for HIDMouse_Middle" in 3DxWare's log). **40 + its own number** makes it
  report in input report 0x03 instead (one bit per button, bit 0 = button 1)
  and move nothing ("sent only to 3DxService": the radial menu, keystrokes).
  Checked by sending 45 for Forward: it then arrived as `03 10`, while Back
  (13) stayed the mouse's own Back. `tdxd` builds the map from the
  `key_V2DK_*` bindings and sends the keys of 0x03 buttons through a uinput
  keyboard. The
  values come from 3DxWare's own log (`<LogLevel>1005</LogLevel>` in the user's
  `Cfg\Global.xml`; the `SBHID_SendPIDConfig_2DMouse` lines). The Speed steps
  are `HIDDev.dll`'s table for the CadMouse family (c651–c658, c65e). Pointer
  acceleration and wheel lines per notch are Windows settings
  (`SystemParametersInfo`), not sent to the mouse.

- **Pairing** (`3DxPair.exe`, EuclidControl / UniversalReceiver): feature
  report 0x41 on the receiver.

  | Command | Bytes | Effect |
  |---|---|---|
  | pairing on | `41 02 MODE 00 00` | MODE (3DxPair's PairingMode): 2 EuclidOnly for a CadMouse, 1 RoundRobin for a SpaceMouse Wireless / Pro Wireless, 6 Integrated for a keyboard |
  | pairing off | `41 02 00 00 00` | 3DxPair sends it once a device has paired, or after 55 s |
  | free a slot | `41 04 SLOT 00 00` | unpairs the device in SLOT |

  For a CadMouse, 3DxPair's steps are: disconnect its USB cable, then switch
  it off and on. The new device appears in the slot table (0x43 + slot). The
  receiver then drops off USB for about 2 s and comes back; it does the same
  after a slot is freed. Seen with the CadMouse Compact Wireless: freed from
  slot 4, then paired again into slot 4 17.7 s after pairing was turned on.
  Pairing a device that is already paired changes nothing.

Captured with a CadMouse Compact Wireless in slot 4:

```
feature 0x42 (interface 4)  42 59 00 00 48 f7 ba 8b   'Y' = c658
feature 0x43..0x46          43 00 00 00 00 00 00 00   empty
feature 0x47                47 59 00 00 48 f7 ba 8b
input 0x17                  17 34 00                  52 %, not charging
input 0x1b                  1b 00 34 00 6d ff 00 00 00
```

## 8. Open items

**Confirmed on a real SpacePilot Pro** (2026-11-16, see `docs/HARDWARE_TESTS.md`):

- the USB topology (section 2);
- HID reports 1, 2, 3 and 4 (sections 3, 4 and 6), and the button masks from
  `Base.xml`;
- the LCD frame format, ping and brightness (section 5).

The SpacePilot Pro's HID descriptor also declares vendor feature reports
`0x05`–`0x0b`, `0x13` and `0x14`. They are not used by `tdxd`.
- **Unknown:** the purpose of those feature reports.

- **Unknown:** whether the Giotto LED reports work on real hardware. They match the
  decompiled `HIDDev.dll`, but no device was available to test them.
- **Unknown:** whether the Enterprise `{0x11, n}` LCD intensity report works on
  real hardware. No device was available to test it.
- **Unknown:** the colour channel order of the Enterprise LCD (see 6.3). `tdxd`
  assumes standard RGB565 and offers `lcd_swap_rb = yes` if red and blue appear swapped.
- **Unknown:** whether the Enterprise LCD transfer works on real hardware. Its
  deflate payload was checked against Python's zlib, but not against the device.
- **Unknown:** the Giotto LCD's source orientation and channel order (see 6.4), and
  whether its transfer works on real hardware. No device was available.
- **Unknown:** how the Giotto firmware combines the legacy `0x13` direct write
  (6.8) with the buffered `0x20`–`0x24` protocol, for example which buffer it
  lands in.
- **Inferred:** the `0x23` field roles (6.5). The slide calls fix neither which x
  field is source nor which y field is source; `tdxd` uses the conventional
  slide direction. Not confirmed on a real Giotto.
- **Unknown:** how Windows makes the result of a vertical move (6.6) visible,
  since it sends no `0x24` for it. `tdxd` presents buffer A itself.
- **Unknown:** a Giotto LCD brightness command. None was found in `HIDDev.dll`.
- **Unknown:** whether the SpacePilot Pro firmware *requires* the ping before it
  accepts frames. The Windows driver uses it only on resume. `tdxd` sends it on
  attach as a precaution and continues if the device does not answer. On real
  hardware the panel answers the ping, but frames were always preceded by one,
  so whether it is needed is untested.
- **Unknown:** the meaning of the 9 bytes the ping returns. They are ignored on
  Windows and in `tdxd`.
- **Unknown:** LCD soft-button reads (`LCDExtDeviceManagerReadSoftButtonsEx`).
  The SpacePilot Pro reports its keys through HID instead.
- **Untested:** a 3D mouse paired to the Universal Receiver. `tdxd` drives its
  interface like the wired device, assuming it sends the same reports.
