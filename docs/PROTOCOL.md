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
- **Unknown:** the meaning of the other 8 data bytes. They are uninitialised stack on
  Windows, and `tdxd` sends zeros.

## 6. LEDs (observed: `HIDDev.dll`)

`FUN_180033ab0` (`CUSBDev` LED on/off) writes a 2-byte HID **output report**
with `WriteFile` on the HID handle:

```
{ 0x04, state }     state: 1 = on, 0 = off
                    (3 instead of 1 on the Enterprise C633 when a device flag is clear)
```

Giotto (C63C, `-0x39c4`) branches to `FUN_180033cd0` instead. See 6.1.

`tdxd` sends `{0x04, led}` when `led = yes|no` is set (all devices except Giotto).

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
`sp4LCD.bmp`. It sends a G19-style header with opcode `0x13`:
`13 0F len32 00 y16 mx16 h16 w16 0F`, followed by the payload. `tdxd` does not
use this path.

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

- **Inferred:** the field roles. The two calls are only consistent with this
  reading. "mx" is the mirrored column, `1280 - w - x`, matching the rotated
  pixel layout. A device model in `tests/test_core.c` executes the planned
  packets and confirms the on-screen result at offsets 1, 320, 1279, 1280,
  1900 and the last page.

**What `tdxd` does** (`tdx_gio_plan_slide` / `tdx_giolcd_slide`):

- It follows the same sequence. Pages are uploaded only when they change. C is
  not uploaded at offset 0, because it isn't read there.
- The renderer keeps the title bar in rows 0..59 and the key grid inside the
  slide band, so a slide moves the whole grid.
- With `lcd_label_13`..`lcd_label_48` set, the daemon builds 2–4 pages.
  `lcd_slide_button = V3DK_…` slides to the next page at `lcd_slide_step`
  pixels per 25 ms, then settles with a full static frame so the page dots in
  the title bar update.
- `tdx-lcd slide P1.ppm P2.ppm [P3 P4]` animates arbitrary pages.

**Not implemented.** Operation type 0 is a vertical variant using `param+0xc` as
y, constants `0x164`/`0x1a0`, and `0x23` copies from D and C without a show.
Operation type 2 only logs.

## 7. Open items

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
- **Not implemented:** the Giotto vertical image operation (type 0 in
  `FUN_180049fd0`), rectangle writes (`0x21`) and the legacy `sp4LCD.bmp` path
  (opcode `0x13`).
- **Inferred:** the `0x23` field roles (6.5). They are consistent with both Windows
  call sites and the device model, but not confirmed on a real Giotto.
- **Unknown:** a Giotto LCD brightness command. None was found in `HIDDev.dll`.
- **Unknown:** whether the SpacePilot Pro firmware *requires* the ping before it
  accepts frames. The Windows driver uses it only on resume. `tdxd` sends it on
  attach as a precaution and continues if the device does not answer.
- **Unknown:** the meaning of the 9 bytes the ping returns. They are ignored on
  Windows and in `tdxd`.
- **Unknown:** LCD soft-button reads (`LCDExtDeviceManagerReadSoftButtonsEx`).
  The SpacePilot Pro reports its keys through HID instead.
