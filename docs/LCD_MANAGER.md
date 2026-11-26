# LCD Manager (Linux port of Logitech LCDMon 3.06)

On Windows, 3DxWare installs Logitech's **LCD Manager** (`LCDMon.exe`, from
`LADPInst.msi`) to run *applets* on the SpacePilot PRO's 320×240 LCD:
Function Keys, RSS Feeds, Picture Viewer. The **LCD Manager** button in the
3DxWare panel opens its window. 3DxLinux replaces it with:

| Windows | Linux |
|---|---|
| `LCDMon.exe` (manager + tray UI) | `tdx-lcdmgr` (user service) + `tdx-lcdmanager` (window) |
| `LgLcdApi.dll` + `lglcd.lib` (applet API, named pipe `\\.\pipe\LGLCDPIPE…`) | `libtdxlcd` (same `lgLcd*` functions, C) and `tdxlcd.py` (Python), Unix socket |
| `LCDExtDevMngrEmulator.dll` / `EDMBTDD.dll` (SpacePilot PRO as a 320×240 "ext. device") | `lcd.c` over usbfs (frames, brightness, ping, **soft buttons**) |
| `%LOCALAPPDATA%\Logitech\LCD Software\LCD Manager\LGLCDDB.TXT` | `~/.config/3dxlinux/lcdmgr.ini` (same sections and keys) |

```
applets (libtdxlcd / tdxlcd.py) ──► $XDG_RUNTIME_DIR/tdx-lcd.sock ──► tdx-lcdmgr ──usbfs──► LCD
tdx-lcdmanager (window) ──admin commands──┘        │  "lcd-release" (held)
                                                  └──► tdxd control socket: tdxd stops drawing
                                                       its key page; takes the LCD back on close
```

Start it with `systemctl --user enable --now tdx-lcdmgr`. While it runs, `tdxd`
leaves the SpacePilot PRO LCD alone (`tdxctl status` shows `lcd lcdmgr`); stop it
and `tdxd`'s built-in key page returns. The Enterprise and Giotto LCDs are not
LCDMon devices (3DxWare drives them itself) and stay with `tdxd`.

## Recovered behaviour

**Soft buttons.** The LCD's navigation keys arrive on USB interface 0, interrupt
IN endpoint `0x81`, 2-byte reports. EDMBTDD.dll `FUN_180001af0` maps byte 0 to
the lglcd button bits:

| bit | lglcd | | bit | lglcd |
|---|---|---|---|---|
| 7 | UP `0x1000` | | 3 | OK `0x0400` |
| 6 | DOWN `0x2000` | | 2 | MENU `0x4000` |
| 5 | LEFT `0x0100` | | 1 | CANCEL `0x0800` |
| 4 | RIGHT `0x0200` | | 0 | `0x80000000` (the LCD key, kept by LCDMon) |

Read exactly one packet (wMaxPacketSize, 2 bytes) per transfer, as LGPBTDD.sys
`FUN_00012434` does: a longer read waits for several full packets. All eight
keys were verified on the SpacePilot PRO (docs/HARDWARE_TESTS.md).
`tdx-lcd buttons` prints them live.

**Program list on the LCD** (LCDMon `FUN_14007fadc`, `FUN_140084a10`,
`FUN_14007fe10`): the `0x80000000` key opens the list of running programs (or,
with *Enable Quick Switch*, goes straight to the next one); UP/DOWN move, OK runs
the highlighted program, CANCEL closes, and the list closes after 2 s without
input. Every other button goes to the program being shown
(`lgLcdOnSoftButtonsCB`, `lgLcdReadSoftButtons`).

**Which program is shown** ("Display Switching Method"):

1. an `LGLCD_PRIORITY_ALERT` (255) update is shown at once when *Immediate
   Display of High Priority Items* (`Alerts`) is on;
2. a program that called `lgLcdSetAsLCDForegroundApp(…, YES)` is shown if
   *Allow Program to Force Itself to Foreground* (`L`) is ticked;
3. `Algorithm=1` (*Manual*): the program picked on the device (`ManualModeApplet`);
   `Algorithm=2` (*Rotate Between Programs*): the next program every `Delay`
   seconds (1–20). *Klever-VU* (`Algorithm=3`) is greyed out for colour panels in
   LCDMon and treated as rotate.

Programs with priority `IDLE_NO_SHOW` (0) are skipped. Mono (160×43) applets are
shown scaled 2× on the colour panel.

**Database** (`FUN_140032680` writes, `FUN_140034f0c` / `FUN_140036c4c` read):
`[DEVICE_0]` `Brightness`, `Contrast` (0–2), `Alerts` (default 1), `Algorithm`
(1–3, default 1), `Delay` (1–20, default 8), `QuickSwitch` (0), `InvertColors`
(0), `ManualModeApplet="path","name"`; `[ClientApplications]`
`N="path","name",E1,A1,S1,L1,C2` with E enabled (default 1), A autostart (0),
S persistent (0), L allow foreground (1), C capabilities; `[Application_N]`
`0=F<family>,I<device>,E<enabled on the device>`. Only persistent programs are
saved. LCDMon's own file can be copied over and read as is (Windows paths are
kept as text).

*Inferred, not read from code:* `S` = the applet's `isPersistent` (LCDMon saves
only those) and `L` = the *Allow…Foreground* box (its default and the window
agree). The default brightness for a new database is 100 (LCDMon takes it from
the device).

## Applet API

`#include <tdxlcd.h>`, link `-ltdxlcd -pthread`. The functions, structures and
constants have the lglcd names (`lgLcdInit`, `lgLcdConnectEx`,
`lgLcdOpenByType`, `lgLcdUpdateBitmap`, `lgLcdReadSoftButtons`,
`lgLcdSetAsLCDForegroundApp`, `lgLcdClose`, `lgLcdDisconnect`, …), so a Windows
applet's LCD code ports by changing the include; strings are UTF-8. Bitmaps are
`LGLCD_BMP_FORMAT_QVGAx32` (320×240, B G R A) or `160x43x1`. Callbacks run on the
library's reader thread. `src/tdx-lcd-demo.c` is a complete example.

Python:

```python
import tdxlcd
conn = tdxlcd.Connection("My Applet", persistent=True, autostart=True,
                         on_configure=show_settings, on_notify=on_event)
dev = conn.open(tdxlcd.DEVICE_QVGA, on_buttons=on_buttons)
dev.update_rgb(rgb_bytes, tdxlcd.PRIORITY_NORMAL)   # 320*240*3 bytes
```

The manager identifies a program by its executable (or, for Python and other
interpreters, its script) and friendly name, like LCDMon's path + name pair.
Programs marked *Automatically Start This Program* are started by the manager.

## Administration

`tdx-lcdmanager` uses these text commands (`tdxlcd.admin()`), also handy from a
shell (`python3 -c 'import tdxlcd; print(tdxlcd.admin("status"))'`):

| Command | Effect |
|---|---|
| `status` | device, settings, programs (index, name, path, E A S L C, device, running, configurable) |
| `set KEY VALUE` | `brightness algorithm delay quickswitch alerts contrast invert show_tray_icon restore_size show_new_device device_balloons warning_balloons` |
| `select N` | show program N (as if chosen on the device) |
| `program N enabled\|device\|autostart\|foreground 0\|1` | the Programs page check boxes |
| `program N configure` / `start` / `remove` | *Configure…*, start now, forget |
| `identify` | show the device name on the LCD for 3 s (*Identify*) |
| `defaults` | device settings back to defaults (*Defaults*) |
| `press HEX…` | inject soft-button states (testing) |

`tdx-lcdmgr -F frame.ppm` runs with a virtual LCD (frames written to a PPM file);
`make test-lcdmgr` uses it to test the manager end to end without a device.

## The window

`tdx-lcdmanager` reproduces LCDMon's window: the device page (*Configuration*,
*Programs Running on This Device*), *Programs* (*Programs on This System*,
*Configure…*, *Program Properties*), *Global Settings*, *Help* and *About*. With
the artwork extracted from your own installer (`tdx-extract-3dxware-ui`, which
now also copies LCDMon's `IDR_*` images) it uses LCDMon's sidebar icons, pane
headers and textures; without it the same layout is drawn plainly. The tray icon
and balloon options are stored but Linux has no LCDMon tray icon yet.

## The program list on the LCD

LCDMon draws its own list of running programs ("Finder") and skins it from
`FinderConfig.txt` in 3Dconnexion's LCD applet package (`BrowserArtPath`):
`client_list_header.png` (the "SpacePilot PRO" bar with the cube),
`client_list_background.png`, `vscroll_*.png`, Arial 10 in 204,204,204, icon and
text offset 36, rows 40 px (LCDMon clamps them to 0-120, 7-120 and 36-72).
Each row shows the program's icon (LCDMon took it from the .exe; Linux applets
send it with `tdxLcdSetIcon()` / `Connection.set_icon_png()`) and its name.
`tdx-lcdmgr` reads the same files from the extracted artwork
(`lcdapplets/finder/`); without them it draws a plain version of the same list.
Rows follow the Programs list order. Text uses an anti-aliased DejaVu Sans
table (`src/font_aa.c`, `make font`) so the static daemon needs no font library.

## The applets

The Windows applets are native x64 programs (Visual Studio 2012, GDI+) from
`3DxLcdApplets64.msi`; the Linux ports are Python programs on `tdxlcd.py` and
cairo/Pango that use the original artwork when it has been extracted
(`tdx-extract-3dxware-ui` copies their bitmaps, PNGs and icons to
`lcdapplets/<applet>/`). Each registers itself (persistent, autostart) and
appears in the LCD Manager's Programs list; on its first start `tdx-lcdmgr`
registers the installed ones in the Windows order.

| Windows | Linux | Notes |
|---|---|---|
| `3DxLCD10SPPRO.exe` "Function Keys" | `3dxlinux-lcd-functionkeys` | Three pages: keys 1-5 with the long-press keys 6-10 in blue; the views (T/B, F/BK, R/L, ISO1/ISO2, Roll); SHIFT ESC CTRL ALT MENU, speed, Pan/Zoom, Dominant, Rotation, FIT. LEFT/RIGHT switch pages. Header: the focused application's icon and name and the filter states (green on / red off). Speed and button-press pop-ups. |
| (3DxService writes `LCDCfg.xml`) | `tdxd` writes `/run/3dxlinux/LCDCfg.xml` (`lcd_cfg`) | The same schema: Device, Filters, InfoLine (Text, Slider, ButtonPressed, ApplicationName, Image), and a `<Button><V3DK>…<Text>…` per button. The text is the explicit `lcd_label_N`, else the driver action, else the key binding (preset name or `Ctrl+Shift+P`), else the button's own function. Button presses and speed changes show for 3 s. |
| `LcdLabelHelper.exe` | built into `tdxd` | Not a 3Dconnexion applet: a helper (MIT, 2026) that restores the button texts in `LCDCfg.xml` under 3DxWare 10.8 and later. `tdxd` writes them itself. |
| `zz3DxLCD50RSSReader.exe` "RSS Feeds" | `3dxlinux-lcd-rss` | RSS 2.0, RSS 1.0 (RDF) and Atom. One item at a time (title, date, text), next item every *Display duration* seconds; UP/DOWN items, LEFT/RIGHT feeds, OK opens the link. Configure…: the feed list (URL, optional short name), display duration, feed icon. Messages: *No Feeds Found!*, *Download failed*, *Invalid Rss Feed*. `~/.config/3dxlinux/rss.json`. |
| `zz3DxLCD60Picture.exe` "Picture Viewer" | `3dxlinux-lcd-pictures` | A folder (optionally subfolders, optionally random), every *Interval* seconds, transitions None / Fade / Wipe / Circle Wipe / Scatter or random; LEFT/RIGHT previous/next, OK pause/play. Default folder: 3Dconnexion's sample pictures. `~/.config/3dxlinux/pictures.json`. |

Configure… in the LCD Manager opens each applet's settings dialog (GTK).
The text positions over the original page backgrounds were placed against the
artwork (the applets' drawing code was not traced line by line); the settings,
messages and behaviour come from their resources, dialogs and decompiled code.
