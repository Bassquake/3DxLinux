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

Giotto (C63C, `-0x39c4`) branches to `FUN_180033cd0` instead. That path, which also
carries per-button RGB colour (`SetLEDColor`, `ButtonLEDColor`, `RingLEDColor`,
`<LEDNumber>` in `Base.xml`), was not recovered.

`tdxd` sends `{0x04, led}` when `led = yes|no` is set.

For the SpacePilot Pro, `HIDDev.dll` `FUN_180034060` also notifies the Logitech LCD
Monitor window (`PostMessage(WM_CLOSE)`) on device changes. This confirms that the
LCD is owned by the Logitech stack and not by `HIDDev`.

## 7. Open items

- **Unknown:** the Giotto LED/colour feature-report format.
- **Unknown:** whether the SpacePilot Pro firmware *requires* the ping before it
  accepts frames. The Windows driver uses it only on resume. `tdxd` sends it on
  attach as a precaution and continues if the device does not answer.
- **Unknown:** the meaning of the 9 bytes the ping returns. They are ignored on
  Windows and in `tdxd`.
- **Unknown:** LCD soft-button reads (`LCDExtDeviceManagerReadSoftButtonsEx`).
  The SpacePilot Pro reports its keys through HID instead.
