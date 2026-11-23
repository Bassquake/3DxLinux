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

## 2026-11-18: the 0.3.1 `.deb` on a real desktop (Kubuntu 26.04 LTS)

Same SpacePilot PRO, plugged into a Kubuntu 26.04 LTS PC (KDE Plasma, user
session), `tdxd_0.3.1-1_amd64.deb` installed.

| Test | Result |
|---|---|
| Service | **Pass:** `tdxd.service` enabled and running from `/usr/bin/tdxd`; `tdxctl status` (as the user) shows the device on `/dev/hidraw2`, firmware 4.11, `lcd lcdmgr`. |
| Settings panel | **Pass:** shows the device as connected. |
| LCD | **Fail, then pass after a fix:** the LCD kept its idle "SpacePilot PRO" picture. The udev rule was `99-3dconnexion-tdxd.rules`, after `73-seat-late.rules`, so its `uaccess` tag never became the user's ACL and tdx-lcdmgr (running as the user) could not open the LCD. Copied as `/etc/udev/rules.d/70-3dconnexion-tdxd.rules` plus `udevadm trigger` and restarting tdx-lcdmgr: the LCD works. Fixed for the next release (rule renamed to `70-`). |
| LED toggle in the panel | Not a fault: `/etc/tdxd.conf` is root's, so the panel saves (one password) when its window closes and tdxd applies the LED then. |

### Upgrade to the test build (same Kubuntu 26.04 PC)

| Test | Result |
|---|---|
| Upgrade 0.3.1 → `0.3.2+1+g1417a66` | **Fail, then pass after a fix:** afterwards tdxd was enabled but stopped (`inactive (dead)`), so nothing detected the device; the package only *try-restarted* tdxd, which leaves a stopped one stopped. Now it restarts (also starts) tdxd and the users' LCD Managers. With `0.3.2+2+gaa46a75` the device is detected again. Why tdxd had stopped is unknown (the system journal needs root to read). |
| Settings apply immediately | **Pass:** toggling LEDs in Advanced Settings asked for the password (pkexec) and the LED changed straight away, with the window still open. |
| Menu after uninstall/upgrade | **Fixed:** "3D Mouse Settings" and "LCD Manager" stayed in the menu from the importer's copies in `~/.local/share/applications` (they win over the package's entries and nothing removes them). The entries and those copies now carry `TryExec=`, so the menu hides a copy once its program is gone. |

## 2026-11-18: uninstall leaves nothing behind (Ubuntu 22.04, WSL)

The 0.3.3 `.deb` installed, then the files users and tools create: an edited
`/etc/tdxd.conf`, a profile in `/etc/tdxd.d`, `~/.config/tdxd`, a per-user and a
system-wide 3DxWare-look import with its icons and menu copy, Python's
`__pycache__`. After `apt-get remove tdxd` (Discover's Uninstall, no purge) a
search of the whole filesystem found nothing of tdxd; unrelated files in the
same folders were kept. dpkg lists the package as `rc` (its record of the
config files), which `apt purge tdxd` clears. An upgrade over it kept the
config edit, the profile and the applet settings.

## 2026-11-18: focus tracking on KDE Plasma (Kubuntu 26.04, Wayland)

Test build `0.3.3+1+g293f906` on the Kubuntu 26.04 PC with the SpacePilot PRO.
Before, the LCD header always said "default": the packages didn't start
`tdx-focus`, and it only knew X11 and sway (under Wayland, `xprop` sees XWayland
windows only).

| Test | Result |
|---|---|
| Focus tracking | **Pass:** with `tdx-focus` started by the package and its new KWin path (`tdx-focus-kwin` loads a KWin script that reports each activated window over D-Bus), the Function Keys header follows the app switched to. |

## 2026-11-18: SpacePilot PRO DOM / PAN ZOOM keys (Kubuntu 26.04)

| Test | Result |
|---|---|
| DOM and PAN ZOOM keys | **Fail, then pass after a fix:** with `tdxctl status`, pressing DOM flipped `panzoom` and PAN ZOOM flipped `dominant`. The DOM key sends `0x08000000` and PAN ZOOM `0x10000000`, swapped from 3DxWare's `Base.xml` (whose defaults in `SharedCfg.xml` match tdxd's actions). Corrected for the SpacePilot PRO (`MASK_FIXES` in `tools/extract_devices.py`); with test build `0.3.3+3+gfdd0812` each key switches its own setting. |

## 2026-11-18: FreeCAD on Kubuntu 26.04 (view keys, cursor)

| Test | Result |
|---|---|
| View keys in FreeCAD | **Pass after a fix:** Top, Front, Right, ISO1 and Roll + did nothing (FreeCAD gets only numbered spacenav buttons and has no default commands for them). The new `/etc/tdxd.d/freecad.conf` sends FreeCAD's own shortcuts while it has the focus; all work. |
| Cap navigation in FreeCAD | **Pass:** works after restarting FreeCAD (installing a package restarts tdxd and FreeCAD does not reconnect to the spacenav socket). |
| Mouse cursor | **Pass after two fixes:** the cursor moved with the cap. The kernel's own input device for the SpacePilot (`3Dconnexion  SpacePilot PRO`) is classed `ID_INPUT_MOUSE`, so libinput used it as a pointer; the udev rule now sets `LIBINPUT_IGNORE_DEVICE` on it. That only took effect after removing the old workaround copy `/etc/udev/rules.d/70-3dconnexion-tdxd.rules`, which shadowed the packaged rule (the packages now remove such copies). tdxd's own 3D mouse is now a separate joystick-only device (`ID_INPUT_JOYSTICK`) and the keys a separate keyboard. |

## 2026-11-18: DOM / PAN ZOOM correction (WSL, SpacePilot PRO)

**Correction to the entry above:** the DOM and PAN ZOOM keys were never
swapped. Raw report 0x03 from the device: DOM sends `0x10000000`, PAN ZOOM
`0x08000000`, as 3DxWare's `Base.xml` says. What was swapped were the Function
Keys header icons (Dominant = bitmaps 114/115, Pan/Zoom = 116/117) and their
order; with wrong icons the right keys looked swapped, and the key "fix" in
0.3.4 made DOM toggle pan/zoom. Undone (`52505f4`), icons fixed (`9a71efe`).

| Test | Result |
|---|---|
| Header icons vs Windows | **Pass:** Rotation, Pan/Zoom, Dominant, left to right, with the Windows pictures. |
| DOM / PAN ZOOM | **Pass:** DOM switches the Dominant icon and setting, PAN ZOOM the Pan/Zoom ones (`tdxctl status`). |

## 2026-11-18: every key, and keys 1-10 with 3DxWare's defaults (WSL, SpacePilot PRO)

Raw report 0x03, pressing every key around the device: MENU, FIT, T, R, F,
Roll +, 1-5, ESC, ALT, SHIFT, CTRL, ROT, PAN ZOOM, DOM, +, − each send the bit
`devices_gen.c` (Base.xml) gives that key. The firmware sends keys 6-10
itself when 1-5 are held past about half a second.

| Test | Result |
|---|---|
| Keys 1-5 (taps) | **Pass:** New Ctrl+N, Open Ctrl+O, Save Ctrl+S, Copy Ctrl+C, Cut Ctrl+X on the "(tdxd keys)" device. |
| Keys 6-10 (hold 1-5) | **Pass:** Paste Ctrl+V, window left Super+Left, window right Super+Right, switch app Alt+Tab, Undo Ctrl+Z. |
| MENU → settings panel | **Pass** on the Kubuntu 26.04 PC (KDE Plasma, test build `0.3.4+5+g69a5e51`): with `action_V3DK_MENU = settings`, MENU opens tdx-settings on the desktop. (Not testable in WSL: root has no desktop session there.) |

## 2026-11-18: focus on the settings panel (Kubuntu 26.04)

| Test | Result |
|---|---|
| Settings panel focused | **Pass after a fix:** the LCD header said "python3.14" (the panel is a Python script). tdxd now names a script by its script and ignores focus on its own windows; with test build `0.3.4+8+gf6e1e2a` the header keeps the previous application. |

## 2026-11-18: FreeCAD keys follow the workbench (Kubuntu 26.04, Flatpak FreeCAD)

| Test | Result |
|---|---|
| Keys per workbench | **Pass after a fix:** with test build `0.3.4+10+gfe16476` nothing changed: FreeCAD is the Flathub Flatpak (`org.freecad.FreeCAD`), which never loaded the add-on from `~/.local/share/FreeCAD`, can't reach `/run/tdxd.sock` and has its own PIDs. With `0.3.4+11+g4310c7a` (`tdx-freecad-addon install` into `~/.var/app/…`, socket override, PID from `SO_PEERCRED`) the LCD shows the active workbench's 3DxWare key names (Part Design key 1 "Create sketch") and the keys run FreeCAD's commands. |

## 2026-11-18: imported 3DxWare application profiles, Firefox (Kubuntu 26.04)

| Test | Result |
|---|---|
| Firefox profile (`3dx-firefox`) | **Pass after a fix:** with test build `0.3.4+13+g1d881ba` the keys did Firefox's functions, but the LCD showed the base config's numbered names (`lcd_label_3`, here an early `/etc/tdxd.conf`: key 3 "Front", key 10 "Paste"). With `0.3.4+14+g5e5dd7f` (a profile's key name beats the base's number) key 3 reloads the page and says "Refresh"; hold 5 (key 10) opens a private window and says "In Private Browsing". |
| Key 2 in Firefox | As expected: 3DxWare's Firefox settings leave key 2 alone, so it keeps the base config's function and name (on this PC still an early config's "Top"). |

## 2026-11-19: FreeCAD key names in the Buttons window, Restore Defaults (Kubuntu 26.04)

| Test | Result |
|---|---|
| Buttons window, FreeCAD focused | **Pass** with test build `0.3.4+17+g7b1abf8`: keys 1-10 show the add-on's names for the workbench (`tdxctl app-keys`), matching the LCD; before, "Button 1" etc. |
| Restore Defaults in the FreeCAD profile | **Fail, fixed:** cleared the profile's view keys (Top, Fit … did nothing). Restored by copying `/usr/share/tdxd/defaults/freecad.conf`; `57b7842` makes Restore Defaults restore a profile's shipped keys. |
| tdxd restarted while FreeCAD runs | Keys 1-10 lost FreeCAD's names and functions (tdxd forgot the add-on's list, FreeCAD's spacenav connection closed); view keys kept working (keyboard shortcuts). **Pass** after restarting FreeCAD. `599576f`: the add-on resends the names and hints to restart FreeCAD. |

## 2026-11-19: settings windows on KDE Plasma Wayland (Kubuntu 26.04)

| Test | Result |
|---|---|
| Window buttons | **Pass after a fix:** the settings windows (main, LCD Manager …) showed KWin's minimize/close as well as 3DxWare's own (GTK 3 asks Wayland compositors to decorate undecorated windows). With test build `0.3.4+21+gd4e625e` (empty hidden title bar, client-side decorations) only the panel's own buttons remain. |

## 2026-11-19: one settings panel per session (Kubuntu 26.04)

| Test | Result |
|---|---|
| System > 3D Mouse Settings clicked repeatedly | **Pass after a fix:** each click opened another panel. With test build `0.3.4+23+g58b60b5` (session bus name org.tdxd.Settings) only one panel opens; further clicks bring it forward. The LCD Manager got the same (org.tdxd.LcdManager): **Pass** with `0.3.4+24+g59b676a`, one window, further starts bring it forward. |

## 2026-11-19: settings panel follows the application (Kubuntu 26.04)

| Test | Result |
|---|---|
| Switching applications with the panel open | **Pass** with test build `0.3.4+26+g582152c`: the panel (and its Buttons window) switches to the profile of the application used last, without closing and reopening it. |

## 2026-11-19: Flatpak application icon on the LCD (Kubuntu 26.04)

| Test | Result |
|---|---|
| FreeCAD (Flathub) icon in the Function Keys header | **Pass after a fix:** no icon (org.freecad.FreeCAD.desktop runs `flatpak run`, entries in Flatpak's exports folder); Firefox etc. were fine. With test build `0.3.4+28+gefe50e3` FreeCAD's icon shows. |

## 2026-11-19: settings panel and device hotplug (Kubuntu 26.04)

| Test | Result |
|---|---|
| Unplug / replug with the panel open, and panel opened while unplugged | **Pass after a fix:** the panel read tdxd's device list only when it opened. With test build `0.3.4+30+gc2f5836` it shows "No device connected" when unplugged and the SpacePilot Pro again when plugged in, without a restart. |

## 2026-11-19: tray icon (Kubuntu 26.04, KDE Plasma Wayland)

| Test | Result |
|---|---|
| `tdx-tray` | **Pass** with test build `0.3.4+32+gba8ebe0`: the 3Dconnexion icon shows in Plasma's system tray; left click opens the settings panel; the right-click menu's Open 3Dconnexion Settings and Write Log File work. |

## 2026-11-19: tray Quit and Start with the Desktop (Kubuntu 26.04, KDE Plasma Wayland)

| Test | Result |
|---|---|
| Quit, Start with the Desktop (tray), Start-up tick box (About) | **Pass** with test build `0.3.4+35+g7641329`. Breeze drew the check item's box in a column left of the icons; with `0.3.4+36+g10eb478` (the tick box drawn as the entry's icon) it lines up with them. |
| Settings panel after Quit | **Pass** with test build `0.3.4+38+g1c78b45`: opening System > 3D Mouse Settings (tdxd) brings back the tray icon and the LCD Manager. |

## 2026-11-22: settings app renamed (Kubuntu 26.04)

| Test | Result |
|---|---|
| Menu entry name | **Pass** with test build `0.3.4+40+g46f3d8b`: the System menu shows "3Dconnexion Settings" (was "3D Mouse Settings (tdxd)"), also with the 3DxWare-look importer's copy of the entry in the home folder. |
| LCD Manager menu entry | **Pass** with test build `0.3.4+42+gbbea7c7`: "LCD Manager (tdxd)" is gone from the menu (entry hidden with NoDisplay, also the importer's copy); the LCD Manager opens from 3Dconnexion Settings. |
| No-device page, More menu, About 3DxWare | **Pass** with test build `0.3.4+44+gf87b0a8`: with the SpacePilot unplugged 3Dconnexion Settings shows 3DxWare's "No device found" page and switches back when it is plugged in; More opens 3DxWare's menu (Import / Export / Reset all settings, Manuals greyed, Support, About); About is titled "About 3DxWare". |
| Package name in Discover | **Pass** with `3DxLinux.0.3.4+46+g570f068-1.amd64.deb`: Discover's install page shows "3DxLinux" (it names a local file by its file name up to the first dot; was "tdxd_0"). Its "Unknown author" can't be set for a local file; the AppStream data (3DxLinux by Bassquake, MIT) is for installed / repository packages. |
| About 3DxWare layout | **Pass** with test build `0.3.4+48+g95e9926`: more space between Configuration, 3DxWare Look, Start-up and Devices; the note says "3DxLinux is an independent Linux driver". |
| More menu look (dark KDE theme) | **Pass** with test build `0.3.4+50+g09e3fe4`: no gap before the icons (GTK's tick-box column off) and 3DxWare's white menu, so its black icons show in a dark theme. |

## 2026-11-22: Universal Receiver and CadMouse Compact Wireless (WSL2)

The receiver (256f:c652, firmware 1.06) passed to WSL2 with usbipd-win, a
CadMouse Compact Wireless (c658) paired to it.

| Test | Result |
|---|---|
| Interfaces | Five HID interfaces (`/dev/hidraw0`–`4`), each with the vendor collection of feature reports 0x41–0x47, 0x50, 0x60, 0x1A; interface 4 also has the CadMouse's mouse (0x1B), button (0x03) and battery (0x17) reports. |
| Pairing table | Feature 0x47 (slot 4) = `59 00 00 48 f7 ba 8b`: code 'Y', the CadMouse Compact Wireless; 0x43–0x46 empty. Feature 0x42 on interface 4 returns the same entry. |
| Battery | Input report `17 34 00` (52 %, not charging) about every 1.5 s while the mouse was used. `HIDIOCGINPUT` for 0x17 fails with EPIPE. |
| `tdxctl status` | **Pass** with test build `0.3.4+53`: `receiver Universal Receiver [256f:c652] firmware 1.06` and `paired 4 CadMouse Compact Wireless [256f:c658] id 0048f7ba8b battery …`; the battery is logged once the mouse moves. |
| Settings pages (rendered in WSL from the live status) | The device chooser lists the CadMouse and the Universal Receiver with their photos; the CadMouse page shows the receiver icon lit and the battery; the receiver page shows "Manage wireless device". |
| Kubuntu 26.04, `3DxLinux.0.3.4+54+g4a32b28` | **Fail:** the chooser listed the CadMouse while it was not connected (the pairing table lists it either way). |
| Mouse switched off and on (WSL, feature reports polled every 0.5 s) | No feature report changes; the only sign is that no input reports arrive while it is off. |
| Only devices heard from (WSL, settings window through WSLg) | **Pass** with test build `0.3.4+54` + fix: after a tdxd restart the window shows just "Universal Receiver"; moving the CadMouse turns the name into the chooser with the CadMouse and the receiver. |

## 2026-11-22: CadMouse settings sent to the mouse (WSL2, through the Universal Receiver)

| Test | Result |
|---|---|
| 3DxWare's log (Windows, `LogLevel` 0x1005) | Speed fully left / right / default: DPI 200 / 7200 / 1400; Polling Rate 125 / 500 / 1000 Hz: divider 8 / 2 / 1; Smart Scrolling off: "normal wheel mode"; Swap Middle / Right: button map `10 12 11 …`. |
| Default block from Linux (`HIDIOCSFEATURE` 0x10 on `/dev/hidraw4`) | Accepted (32 bytes); the mouse behaves as before. |
| DPI from Linux | The same movement three times: 51,559 counts at 1400 dpi, 6,965 at 400, 92,455 at 7200 (about 36, 17 and 170 per report). The mouse applies the DPI. |
| tdxd + 3Dconnexion Settings (WSLg) | **Pass** with test build `0.3.4+55` + changes: the CadMouse page's Speed slider saves `mouse_dpi`, and tdxd sends 7200 dpi when the mouse wakes; Advanced Settings (3DxWare's layout, acceleration and wheel speed greyed) saves 500 Hz and Smart Scrolling off, which tdxd sends; Restore Defaults puts them back. |
| Button map from Linux | Forward set to 45 (40 + 5): it arrives as input report `03 10`; the radial-menu button (47) as `03 40`; Back (13) and the wheel click (12) stay the mouse's own (`1b 08`, `1b 04`). |
| CadMouse Buttons window (WSLg) | **Pass** with test build `0.3.4+56` + changes: Forward = Undo and the radial-menu button = Middle Mouse Function: tdxd sends `10 11 12 12 45 13 12`; Swap Middle / Right sends `10 12 11 …`; each Forward press comes out as Ctrl+Z (down, up) on tdxd's keyboard device, which exists before the first press. |
| Kubuntu 26.04, `3DxLinux.0.3.4+57+g3d64bab` | **Fail:** the CadMouse was detected but did not move the cursor: the udev rule hid every 256f input device from libinput (meant for the 3D mice), the Universal Receiver too. Fixed: the rule skips 256f:c650-c66f (CadMouse, keyboards, Universal Receivers). |
| Kubuntu 26.04, `3DxLinux.0.3.4+58+ga2de5a8` | **Pass:** after the receiver was plugged in again, the CadMouse moves the cursor. |
| CadMouse settings on Kubuntu (`3DxLinux.0.3.4+58+ga2de5a8`) | **Pass:** Speed changes the pointer speed; Smart Scrolling off / on changes the wheel; Forward = Undo undoes in a text editor; Swap Middle / Right swaps them; Application Specific Settings gives the chosen application its own Forward button. |

## 2026-11-22: Add device / pairing (WSL2, Universal Receiver)

| Test | Result |
|---|---|
| Pairing mode on / off for the paired CadMouse | `41 02 02 00 00` / `41 02 00 00 00` accepted; switching the mouse off and on changes nothing (already paired). |
| Free slot 4 (`41 04 04 00 00`) | Slot 4 empty; the receiver drops off USB and is back 2 s later (usbipd `--auto-attach` keeps it in WSL). |
| Pairing a new device | Pairing on, the CadMouse switched off and on: slot 4 = `59 00 00 48 f7 ba 8b` after 17.7 s, then the receiver reconnects. |
| Add device window (WSLg), `tdxctl unpair 4` first | First try: tdxd paired the CadMouse, but the window showed "taking too long" while the receiver was off USB; fixed (the window waits for tdxd's result). **Pass** after the fix: "Your CadMouse Compact Wireless was successfully paired.", `tdxctl status` shows `pairing done 4 CadMouse Compact Wireless`. |
| Add device on Kubuntu 26.04 (`3DxLinux.0.3.4+61+gcc36764`) | **Pass:** after `tdxctl unpair 4`, Add device → CadMouse and switching the mouse off and on: "successfully paired", and the CadMouse works again. |
| Receiver Advanced Settings (WSLg) | **Pass:** "Currently paired devices (1/5)" with the CadMouse tile (PID: Y1224194699, as 3DxWare shows it); the ✕ on hover removed it (0/5), and Add device from that window paired it back. |
| CadMouse Buttons: Application Specific Settings | **Pass** with `3DxLinux.0.3.4+64+gd7144be`: a tick box like Swap Middle / Right instead of the switch. |
| Header icon (WSLg) | **Pass:** the application icon in the title row raised 3 px, level with the title's capitals (was about 4 px low). |
| CadMouse acceleration and wheel lines on Kubuntu 26.04 / KDE Plasma (`3DxLinux.0.3.4+67+g631c7f2`) | **Pass:** Advanced Settings' "Enable acceleration" and "Lines" are usable and work (set through KWin's input devices). |
| CadMouse Advanced Settings label (`3DxLinux.0.3.4+69+ge279646`) | **Pass:** the greyed wheel mode reads "One page / one screen (Windows only)". |
| Device unplugged with windows open (Kubuntu 26.04, `3DxLinux.0.3.4+71+gdde8a86`) | **Pass:** the open windows close and the main window shows what is left. |
| CadMouse Buttons: Application Specific Settings (`3DxLinux.0.3.4+75+g9c7ec97`) | **Pass:** 3DxWare's slider switch (dark grey off, blue on), replacing the tick box. |
| CadMouse Buttons: 3DxWare's action picker (WSLg, demo status) | **Pass:** search, folder sections, the current function highlighted, the grip; after a first try it was clipped inside the Buttons window (a Gtk.Popover), now a pop-up window that reaches past it. |
| CadMouse action picker on Kubuntu 26.04 / KDE Plasma Wayland | `0.3.4+79`: the drop-downs didn't open (the popup was grabbed after being shown; KWin closes such a popup). `0.3.4+80`: they opened, after about 4 s once. **Pass** with `3DxLinux.0.3.4+81+g426c7e6`: `TDX_DEBUG_PICKER=1` shows it grabbed, mapped and drawn within 0.03-0.05 s, and it opens without delay. |
| Action picker's arrow and grip on Kubuntu 26.04 (`3DxLinux.0.3.4+81+g426c7e6`) | **Pass:** the arrow points at the button's row; dragging the dots resizes the list only while held, and follows the pointer. |
| Action picker's search box on Kubuntu 26.04 (dark KDE theme) | `0.3.4+84`: no visible cursor (the theme's light cursor on the white field). **Pass** with `3DxLinux.0.3.4+85+gb41c15e`: the blinking cursor shows when the picker opens. |
| Add device: 3DxPair's Device Selection (Kubuntu 26.04, `3DxLinux.0.3.4+87+ge56d6b7`, 3DxWare look imported again) | **Pass:** "3Dconnexion Pairing / Device Selection" with SpaceMouse Pro Wireless, SpaceMouse Wireless and CadMouse in 3DxPair's pictures, as on Windows. |
| Action picker hover and selection (WSLg, Universal Receiver attached, `0.3.4+91`) | Section headings did not highlight under the real pointer (they only took clicks). **Pass** with `0.3.4+92`: a hovered section is grey and bold, a hovered entry light grey and bold, the selected entry darker grey, as in 3DxWare. |
| CadMouse Buttons: "Type a shortcut…" and the row arrow (WSLg, Universal Receiver attached, `0.3.4+92` + this change) | **Pass:** clicking a row's text shows "Type a shortcut…"; a combination is saved and shown in capitals (SHIFT+K); the arrow shows its box and dark arrow on hover and opens the picker. |
| Common Tools radial menu (CadMouse radial button) | WSLg, `0.3.4+93` + change: the menu opened, but in the top-left corner (the pointer was outside the Linux display; fixed by waiting for it to enter). Kubuntu 26.04 / KDE Plasma Wayland, `3DxLinux.0.3.4+94+g9e71fcd`: opens at the pointer, Undo and Redo work; too large (sized from a 125 % screenshot) and Properties... opened nothing (stopped with the menu's systemd unit). **Pass** with `3DxLinux.0.3.4+95+g4b94c59`: 3DxWare's size, Properties... opens 3Dconnexion Settings. |
