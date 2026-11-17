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

### Blender (libspnav client)

Same day, same device and WSL setup, `tdxd` 0.1.0 with the shipped
`tdxd.conf`. The spacenavd socket was at its default path, `/run/spnav.sock`.

**Blender** is Ubuntu 22.04's package: Blender 3.0.1 (`3.0.1+dfsg-7`), linked
against `libspnav.so.0` (0.2.3). Its window was shown on the Windows desktop
through WSLg, and it ran with `--debug-events` to log incoming events. This
build does not accept `--debug-ndof`.

**Socket path.** libspnav builds its path from the strings `/var/run` and
`/spnav.sock`. `/var/run` is a symlink to `/run`, so this is `tdxd`'s default
socket and no configuration was needed.

**Connection.**

- `/proc/net/unix` showed two endpoints on the socket: `tdxd`'s listener and
  Blender's connection.
- Blender identified the device itself and printed `ndof: using SpacePilot Pro`.
  It finds the model by running `lsusb`, which saw the USB/IP-attached device
  as `046d:c629`.

| Test | Result |
|---|---|
| Viewport navigation | **Pass:** orbit, pan and zoom followed the cap in natural directions. No axis remapping in `tdxd.conf` was needed. |
| Motion events | **Pass:** 79 `NDOF_MOTION` events logged with real values, e.g. `rot (0 −0.4914 0.3657)`, `tx (0 −0.5143 −0.1600)`. |
| View buttons | **Pass:** every press that `tdxd` logged arrived as a press/release pair, as listed below. |
| FIT button | **Pass:** after clicking into the viewport, 8 presses in `tdxd` gave 14 `NDOF_BUTTON_FIT` events, and you confirmed the view re-framed. |

Button presses in `tdxd` and the Blender events they produced:

| Button | Presses in `tdxd` | Blender events |
|---|---|---|
| `NDOF_BUTTON_TOP` | 3 | 6 |
| `NDOF_BUTTON_ROLL_CW` | 3 | 6 |
| `NDOF_BUTTON_FRONT` | 2 | 4 |
| `NDOF_BUTTON_RIGHT` | 2 | 4 |
| `NDOF_BUTTON_ISO1` | 2 | 4 |

**FIT and window focus.** In the first run, two FIT presses produced no Blender
event, even though `tdxd` sent them.

- **Inferred:** this is a focus issue. Blender's log shows a `WINDOW_DEACTIVATE`
  event, and FIT worked every time once the viewport had been clicked. So
  Blender apparently ignores NDOF buttons while its window is unfocused. This
  is Blender behaviour, not a `tdxd` problem.

**Button numbering.** `tdxd` sends spacenavd button numbers in `Base.xml` bit
order. This matches what Blender expects for the SpacePilot Pro: FIT = 1,
TOP = 2, RIGHT = 4, FRONT = 5, ROLL_CW = 8, ISO1 = 10.

### Calibrate and per-application profiles

Same day, same device and WSL setup, with the development build after commit
`d288772` plus the `realpath` fix described below.

**Configuration:**

- the shipped `tdxd.conf` with `calibrate_button = V3DK_MENU`;
- `profile_dir` holding the example `blender.conf` profile, which matches
  `blender, blender-*` and sets labels 1–3 to Edit, Undo and Redo.

| Test | Result |
|---|---|
| Firmware version | **Pass:** `tdxctl status` reported `SpacePilot PRO [046d:c629] /dev/hidraw0 firmware 4.11`, read from sysfs `bcdDevice` = `0x0411`. That is ≥ 0x200, so 3DxWare's re-zero gate allows the command. |
| `tdxctl calibrate` | **Pass:** `ok SpacePilot PRO: re-zeroed`. The device accepted feature report `{0x07, 0x00}`: `HIDIOCSFEATURE` succeeded and the device did not reject the request. |
| Rest readings | No change was visible. The device sent no axis reports during 3 s at rest, both before and after the calibration. This cap was already centred, so the calibration had no drift to remove. |
| `calibrate_button = V3DK_MENU` | **Pass:** each MENU press logged `re-zeroed` (pressed twice). |
| Profile switch via `tdxctl focus blender` | **Pass:** the active profile became `blender (via focus)`, and the LCD switched to keys 1–3 labelled Edit, Undo and Redo (as you saw). Key 1 presses were handled under that profile. |
| Profile switch back (`tdxctl focus gimp`) | **Pass:** the default profile was restored. |
| Input during the test | **Pass:** in a 20 s window there were 28 translation, 28 rotation and 8 button reports on hidraw, and `tdxd` logged every button event. |

**Bug found and fixed in this test.** On its first start with the real device,
`tdxd` aborted with `*** buffer overflow detected ***`.

- **Cause:** the new `bcdDevice` lookup called `realpath()` with a 512-byte
  buffer. glibc's fortified `realpath` requires `PATH_MAX`.
- **Why it wasn't caught earlier:** the device-less test never reached that code.
- **Fix:** the buffer is now `PATH_MAX`, and the daemon then ran normally.
- **Scope:** the v0.1.0 release does not contain this code.

### Not covered by this test

- Whether `tdxctl calibrate` corrects a drifting cap. This device had no rest
  drift to correct.
- `tdx-focus` on a real X11 or sway desktop. Focus was set with `tdxctl focus`.
- Linux applications other than Blender (FreeCAD, etc.).
- Applications using the uinput device rather than the spacenavd socket.
- Pulling the cap upward (the +Z direction).
- Whether the LCD *requires* the ping before frames. The ping was always sent
  first in these tests.
- The SpaceMouse Enterprise and Giotto code paths (no such devices available).

## 2026-11-17: button actions and the `tdx-settings` panel

Same SpacePilot Pro (firmware 4.11) on WSL2 + usbipd, with the build of commit
`0eb8998`. `tdxd` ran with the shipped `tdxd.conf` (sockets and `profile_dir`
moved under `/tmp`), which binds +, −, DOM, LOCK and PAN ZOOM to actions.
`tdx-settings` ran on the Windows desktop through WSLg.

| Test | Result |
|---|---|
| `action_V3DK_PLUS = speed_up` / `action_V3DK_MINUS = speed_down` | **Pass:** each press stepped the speed by ×1.25 (1.00 → 1.25 → 1.56 → 1.25, …); overlapping + and − presses were each handled once. |
| `dominant`, `rotation`, `panzoom` toggles | **Pass:** DOM turned dominant on, LOCK turned rotation off, PAN ZOOM turned pan/zoom off; `tdxctl status` then showed `speed 1.250`, `panzoom off`, `rotation off`, `dominant on`. |
| Main window | **Pass:** showed SpacePilot PRO, firmware 4.11, `/dev/hidraw0`. Moving Speed saved `sensitivity` and reloaded tdxd. |
| Advanced Settings: LEDs Active | **Pass:** unticking turned the LED off, ticking turned it back on. |
| Advanced Settings: Calibrate | **Pass:** tdxd logged `re-zeroed (rest position calibrated)`. |
| Buttons: button 1 → Keyboard › Copy | **Pass:** saved `key_V3DK_1 = KEY_LEFTCTRL+KEY_C`. |
| LCD Manager: brightness | **Pass:** Low dimmed the LCD; High restored it (saved as `lcd_brightness = 90`). |
| LCD Manager: label 1 = "Hello" | **Pass:** the LCD key page showed Hello after the reload. |
| Saving | **Pass:** every change triggered `configuration reloaded`; comments and layout of `tdxd.conf` were kept (only the changed lines differ). |

### Not covered by this test

- The `pkexec` path (WSL runs as root, so files were written directly).
- Whether action buttons are kept away from applications was checked in the
  code path and logs, not with a client listening.
- A per-application profile edited from the panel on real hardware.

## 2026-11-17: LCD Manager (tdx-lcdmgr) and the LCD soft buttons

Same SpacePilot PRO (firmware 4.11), WSL2 + usbipd, build of commit `a8a3af0`
plus the soft-button read-length fix below. tdxd ran as before; `tdx-lcdmgr -v -v`
and two `tdx-lcd-demo -a` applets ran beside it; `tdx-lcdmanager` ran through WSLg.

| Test | Result |
|---|---|
| Handover from tdxd | **Pass:** `tdxd released the LCD`; tdxd logged `LCD handed to the LCD manager (1 released)` and `tdxctl status` showed `lcd lcdmgr`. |
| Handover back | **Pass:** stopping tdx-lcdmgr made tdxd log `LCD manager disconnected; tdxd drives the LCD again` and re-open the LCD. |
| Applet frames | **Pass:** "Demo One" (libtdxlcd, `LGLCD_SYNC_UPDATE`) shown on the LCD with its counter running. |
| Soft buttons (interrupt IN `0x81`) | **Pass after a fix:** all eight LCD keys report, mapped as EDMBTDD.dll does: UP `0x1000`, DOWN `0x2000`, OK `0x400`, LEFT `0x100`, RIGHT `0x200`, MENU `0x4000`, CANCEL `0x800`, and the LCD key `0x80000000`; each release reports 0. |
| Buttons to the applet | **Pass:** with the list closed, UP/DOWN/OK went to the shown applet (its BUTTONS value changed). |
| Program list on the LCD | **Pass:** the LCD key opened the list, DOWN moved the selection, OK switched to "Demo Two"; the list closes after 2 s without input. |
| Autostart | **Pass:** persistent autostart programs in the database were started by the manager. |
| `tdx-lcdmanager` window | **Pass:** Identify (device name on the LCD for 3 s), Brightness (LCD dims and returns), Programs page (both demos Running; Enable unticked and ticked again), Global Settings and About pages. |

**Bug found and fixed in this test.** The first soft-button read asked for 8 bytes;
the endpoint's packets are 2 bytes (wMaxPacketSize), and a full-size packet does
not end an interrupt transfer, so presses never completed the read. LGPBTDD.sys
(`FUN_00012434`) reads exactly one packet; `lcd.c` now uses the endpoint's
wMaxPacketSize too.

### Not covered by this test

- The Windows applets themselves (Function Keys, RSS Feeds, Picture Viewer):
  they are ported next.
- Rotation, alerts and foreground requests on hardware (covered by
  `make test-lcdmgr` with the virtual LCD).
- The user service (`systemctl --user`) and the `pkexec`-less user access
  through the udev `uaccess` rule on a real desktop (WSL runs as root).

## 2026-11-17: the LCD applets (Function Keys, RSS Feeds, Picture Viewer)

Same SpacePilot PRO and WSL2 setup. tdxd wrote `/run/tdxd/LCDCfg.xml`;
`tdx-lcdmgr` started with a new database, registered and autostarted the three
applet ports and used the artwork extracted from the 3DxWare installer.

| Test | Result |
|---|---|
| First run | **Pass:** Function Keys, RSS Feeds and Picture Viewer registered in the Windows order and autostarted; the program list used the extracted Finder skin. |
| Program list on the LCD | **Pass:** looks like the Windows one (photo): "SpacePilot PRO" header with the cube, each applet's icon and name, highlighted row. |
| Function Keys | **Pass:** application name in the header (`tdxctl focus blender`), keys 1-5 with 6-10 in blue; LEFT/RIGHT switch the three pages; +/- show the Speed pop-up; a function key shows its text. |
| RSS Feeds | **Pass:** "No Feeds Found!" without feeds; after adding a feed with Configure... (LCD Manager → Programs) its items appeared on the LCD. |
| Picture Viewer | **Pass:** slideshow of the 3Dconnexion sample pictures with transitions; LEFT/RIGHT, OK pause/play. |
| Picture Viewer mini menu | **Pass:** the MENU key (white square) opens Configure... / Cancel; OK opened the settings dialog on the PC, the back arrow closed the menu. |
| RSS Feeds, OK on a story | **Pass** after a fix: the program list keeps three entries. |
| Function Keys app switch | **Pass:** `tdxctl focus` blender / freecad / gedit with two profiles: LCDCfg.xml and Function Keys follow each time (application name, the profile's labels, base labels for keys it leaves alone). A switch or reload sends nothing to the device unless the LED or brightness changes (strace). |
| Function Keys app icon | **Pass:** Blender's logo (its scalable SVG, found through blender.desktop) in the header. |
| Taskbar icons (WSLg) | **Pass:** the settings window shows the 3Dconnexion logo and the LCD Manager LCDMon's icon, as on Windows; window dragging still works. |

**Fixed during this test:** tdxd only wrote `LCDCfg.xml` after an input event;
it now also writes it on an idle poll.

**Fixed during this test:** OK on an RSS story ran `xdg-open`, which WSL lacks;
the error escaped in the client library's reader thread, which took it for a
lost connection, and the applet reconnected without closing the old one, so
RSS Feeds showed twice in the program list. Callback errors no longer end a
connection, and a dropped connection is closed before reconnecting.

**Fixed during this test:** after the usbip link dropped (Windows already
showed the device detached, WSL still attached), `tdxctl reload` hung tdxd
for good: every profile switch and reload re-sent the LED report, a HID
SET_REPORT that timed out, and the kernel's cancel of it never completed
over the dead link (`D` state in `usb_kill_urb`; freed by detaching port 0
in `/sys/devices/platform/vhci_hcd.0/detach`). tdxd now sends the LED,
LCD brightness and Enterprise intensity reports only when they change.

**Found during this test:** WSLg gives a Wayland window a generic taskbar
icon even when its `.desktop` entry has the right one (WSLg made Start-menu
shortcuts with the correct icons, but sends none for the open window), while
an X11 window's own icon gets through. The GUIs now use X11 under WSLg unless
`GDK_BACKEND` is set. WSLg also only watches `/usr/local/share/applications`
if it existed when WSLg started, only searches `/usr/share/icons` for icon
names, and skips `NoDisplay` entries - so the extractor writes absolute icon
paths into the `.desktop` entries. Wayland on a real desktop is untested.

## 2026-11-17: the 0.3.1 `.deb` package

`tdxd_0.3.1-1_amd64.deb` installed with apt on the Ubuntu 22.04 WSL (systemd
on), then tested with the SpacePilot PRO attached.

| Test | Result |
|---|---|
| Install | **Pass:** only `tdxd` itself is added (Python 3/GTK 3 were present); `tdxd.service` enabled and running, `uinput` loaded, `/dev/hidraw0`, the LCD's USB node and `/dev/uinput` group `input`; the LCD Manager user unit enabled globally. |
| Device | **Pass:** the installed tdxd found the SpacePilot PRO (firmware 4.11) on attach. |
| LCD Manager | **Pass:** `/usr/bin/tdx-lcdmgr` registered and started the three applets from `/usr/bin`; all connected (0.3.0's applets failed here). Started by hand: WSL's root has no desktop session to start the user unit. |
| Upgrade (reinstall) | **Pass:** tdxd restarted, an edit to `/etc/tdxd.conf` kept. |
| Purge | **Pass:** service, user unit, programs, config, udev rule removed; no tdxd left running. |
| 3DxWare look | **Pass:** `sudo tdx-extract-3dxware-ui <installer>` from the package filled `/usr/local/share/tdxd/3dxware-ui` and the icon theme; after restarting tdx-lcdmgr the device showed the Windows look (program list and applets). |
| Import button | **Pass:** tdx-settings → About → **Import 3DxWare look…** imported the installer chosen in the file dialog, reported what it imported, restarted the panel on its About window with the new date. |

**Fixed during this test:**

- The applets didn't find `lcdapplet.py` when installed (also in 0.3.0's
  `.tar.gz`).
- `xdg-utils` and `x11-utils` as *Recommends* pulled in 60 packages on a
  minimal system (Perl libraries, the tilix terminal); now *Suggests*. The 60
  were removed again.
- tdx-lcdmgr didn't pass its socket to applets it started (they missed a
  manager on `-s`), and left them running when it exited.
- Entries for applets that moved (`/usr/local/bin` → `/usr/bin`) now follow
  them.

Not tested: the `.rpm` (no Fedora system), the arm64 packages, and a
double-click install on a real desktop.
