# Hardware test results

## 2026-11-16 — SpacePilot Pro (046d:c629), tdxd 0.1.0

### Setup

- **Host:** Windows 10 IoT Enterprise LTSC 2021, with 3DxWare 10.9.14 installed.
- **Linux side:** WSL2, Ubuntu 22.04, kernel `6.18.40.1-microsoft-standard-WSL2`.
- **Passthrough:** the device is passed to WSL with usbipd-win 5.3.0. See
  "Testing from Windows with WSL2" in the README.
- **Binaries:** the static `tdxd-0.1.0-linux-x86_64` release build.
- **Before testing:** loaded the modules `vhci_hcd usbhid hid_generic uinput evdev`.

### USB topology (seen from Linux)

```
1-1  046d:c629 "SpacePilot PRO"  high speed (480 Mbit/s)
 ├─ 1-1:1.0  class 0xff  ep 0x02 bulk OUT (512)   ep 0x81 interrupt IN (2)    LCD
 └─ 1-1:1.1  class 0x03  ep 0x83 interrupt IN (7)  driver usbhid -> /dev/hidraw0
```

This matches section 2 of `PROTOCOL.md`. Windows showed the same split:
interface 0 under "Logitech LCDs" and interface 1 under HIDClass.

### HID report descriptor (226 bytes)

| Report ID | Type | Size | Usage page |
|---|---|---|---|
| 0x01 | Input | 6 bytes | Generic Desktop (translation) |
| 0x02 | Input | 6 bytes | Generic Desktop (rotation) |
| 0x03 | Input | 4 bytes | Button |
| 0x04 | Output | 1 byte | LED |
| 0x05, 0x06, 0x07, 0x0b, 0x13 | Feature | 1 byte | Vendor 0xFF00 |
| 0x08, 0x09, 0x0a | Feature | 7 bytes | Vendor 0xFF00 |
| 0x14 | Feature | 28 bytes | Vendor 0xFF00 |

The input reports match section 3. The 7-byte maximum packet means translation
and rotation arrive as separate reports, not one combined 13-byte report. The
feature reports are not used by `tdxd`; their meaning is unknown.

### Results

| Area | Test | Result |
|---|---|---|
| Detection | `tdxd -l`, hidraw scan | **Pass:** matched on `/dev/hidraw0` (interface 1) |
| Motion | 180 s capture | **Pass:** 397 × report 1 and 397 × report 2. Observed raw ranges: tx −322…244, ty −334…373, tz −10…169 (pushed down only), rx −454…433, ry −376…484, rz −418…403 |
| spacenavd socket | `/tmp/spnav.sock` client | **Pass:** 794 motion events with values identical to raw |
| uinput | "3Dconnexion SpacePilot PRO (tdxd)" | **Pass:** 1,811 EV_ABS and 24 EV_KEY events |
| Buttons | Press/release log | **Pass:** 12 buttons, each named from the `Base.xml` table: SHIFT, CTRL, ALT, 1–5, MENU, FIT, ISO1, PLUS |
| LCD frame | `tdx-lcd fill bars` | **Pass:** the 154,112-byte frame was accepted in ~0.5 s. The image was correct: colours, bar order, orientation and readable text all as expected |
| LCD ping | `tdx-lcd ping` (vendor IN 0x01) | **Pass:** the panel answers |
| LCD brightness | `tdx-lcd brightness N` (vendor 0x0A) | **Pass, quantised:** dimming was visible. See the table below |
| LED | HID output `{0x04, 0/1}` | **Pass:** a light on the device blinked off and on, four times |
| Daemon + LCD | `tdxd` with `lcd = yes` for 45 s | **Pass:** the key page (keys 1–5) was drawn. Holding SHIFT switched to keys 6–10, and releasing switched back |

#### Brightness levels

The panel has three hardware levels. Each written value snaps to one of them:

| Value set (0..100) | Value read back | Raw byte from the device |
|---|---|---|
| 0, 1, 5, 10, 20, 25, 30 | 29 | 30 |
| 40, 50, 60 | 59 | 60 |
| 75, 89, 100 | 89 | 90 |

The read-back is the raw byte minus one, which is how `EDMBTDD.dll` presents
it. Values 31–39 and 61–74 were not tried, so the exact thresholds are unknown.
The value read back did not change one second after setting it, so the panel
switches levels immediately rather than fading.

### Not covered by this test

- Linux applications reading the spacenavd socket or the uinput device (Blender,
  FreeCAD). Events were only checked with the capture script inside WSL.
- Pulling the cap upward (the +Z direction).
- Whether the LCD *requires* the ping before frames. The ping was always sent
  first in these tests.
- The SpaceMouse Enterprise and Giotto code paths (no such devices available).
