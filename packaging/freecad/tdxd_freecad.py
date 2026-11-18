# SPDX-License-Identifier: MIT
"""tdxd add-on for FreeCAD: the SpacePilot's keys follow the active workbench.

On Windows, FreeCAD (0.22+) and 3DxWare talk through 3Dconnexion's NavLib:
FreeCAD tells the driver which workbench is active, and every workbench has its
own key set (FreeCAD_Navlib.xml) - Part Design: 1 Create sketch, 2 the "Part
Design Modeling" radial menu, 4 Create body, ... On Linux FreeCAD only gets
numbered spacenav buttons, so this add-on does that part:

- one FreeCAD command per SpacePilot key ("Tdxd_Key_V3DK_1", ...), put into
  FreeCAD's own 3D mouse button map (BaseApp/Spaceball/Buttons/<n>/Command), so
  FreeCAD runs it when the key is pressed;
- the command runs what the active workbench's set has for that key: a FreeCAD
  command (with a group command's entry), or a radial menu as a pop-up menu;
- on every workbench change it tells tdxd which keys FreeCAD now handles and
  their names (tdxctl app-buttons): the LCD shows "Create sketch", and tdxd
  does not also send its own shortcut for those keys. Keys a workbench leaves
  out keep tdxd's functions (as on Windows).

The key sets come from your own 3DxWare installer, read by the "Import 3DxWare
look" step (freecad/buttons.json next to the artwork).
"""
import json
import os
import socket

SOCKET = os.environ.get("TDXD_SOCKET", "/run/tdxd.sock")
PREFIX = "Tdxd_Key_"


# ---- pure logic (no FreeCAD needed: tested in tests/test_freecad.py) --------

def asset_dirs():
    env = os.environ.get("TDX_UI_ASSETS")
    return ([env] if env else []) + [
        os.path.expanduser("~/.local/share/tdxd/3dxware-ui"),
        "/usr/local/share/tdxd/3dxware-ui", "/usr/share/tdxd/3dxware-ui",
        # FreeCAD as a Flatpak sees the host's /usr here
        "/run/host/usr/local/share/tdxd/3dxware-ui", "/run/host/usr/share/tdxd/3dxware-ui"]


def load_buttons(dirs=None):
    """The imported FreeCAD key sets, or None (not imported yet)."""
    for d in dirs or asset_dirs():
        p = os.path.join(d, "freecad", "buttons.json")
        try:
            with open(p) as f:
                return json.load(f)
        except (OSError, ValueError):
            continue
    return None


def bank_for(data, workbench):
    """The key set of a workbench; the default one when it has none (Start, ...)."""
    banks = data.get("banks", {})
    return banks.get(workbench) or banks.get("Default") or {}


def key_names(data, workbench, keys):
    """[(V3DK id, name)] for the keys of @keys (the device's) in the workbench's set."""
    bank = bank_for(data, workbench)
    out = []
    for k in keys:
        a = data.get("actions", {}).get(bank.get(k, ""))
        if a:
            out.append((k, a.get("name") or k))
    return out


def app_buttons_message(pid, names):
    """The tdxctl app-buttons line: fields split by tabs (names have spaces)."""
    fields = ["app-buttons", str(pid)]
    for k, n in names:
        fields += [k, n.replace("\t", " ").replace("\n", " ")[:40]]
    return "\t".join(fields)


def resolve(data, workbench, key):
    """What a key does in a workbench: ("command", name, index), ("menu", title,
    [(name, command, index)]), or None (not FreeCAD's: tdxd's own function)."""
    acts = data.get("actions", {})
    a = acts.get(bank_for(data, workbench).get(key, ""))
    if not a:
        return None
    if "menu" in a:
        m = data.get("menus", {}).get(a["menu"], {})
        items = [(acts[i]["name"], acts[i]["command"], acts[i].get("index"))
                 for i in m.get("items", []) if "command" in acts.get(i, {})]
        return ("menu", m.get("name") or a.get("name", ""), items)
    return ("command", a["command"], a.get("index"))


def parse_buttons(reply):
    """tdxctl buttons -> {V3DK id: spacenav button number}."""
    out = {}
    for line in reply.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0].isdigit():
            out[parts[1]] = int(parts[0])
    return out


def tdxd(command, path=SOCKET):
    """One request to tdxd's control socket; the reply, or "" when tdxd is away."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(path)
        s.sendall((command + "\n").encode())
        reply = b""
        while True:
            b = s.recv(4096)
            if not b:
                break
            reply += b
        s.close()
        return reply.decode(errors="replace")
    except OSError:
        return ""


# ---- inside FreeCAD -----------------------------------------------------------

class _Key:
    """A FreeCAD command bound to one SpacePilot key."""

    def __init__(self, addon, key):
        self.addon, self.key = addon, key

    def GetResources(self):
        return {"MenuText": "SpacePilot key %s" % self.key[5:],
                "ToolTip": "Runs what the active workbench has on the 3D mouse key %s "
                           "(tdxd)" % self.key[5:]}

    def IsActive(self):
        return True

    def Activated(self):
        self.addon.press(self.key)


class Addon:
    def __init__(self, Gui, FreeCAD, QtGui, QtCore):
        self.Gui, self.FreeCAD, self.QtGui, self.QtCore = Gui, FreeCAD, QtGui, QtCore
        self.data = load_buttons()
        self.numbers = {}
        self.workbench = ""

    def log(self, text):
        self.FreeCAD.Console.PrintLog("tdxd: %s\n" % text)

    def start(self):
        if not self.data:
            self.FreeCAD.Console.PrintMessage(
                "tdxd: no FreeCAD key sets yet - import the 3DxWare look in tdx-settings "
                "(About)\n")
            return
        self.numbers = parse_buttons(tdxd("buttons"))
        if not self.numbers:
            self.FreeCAD.Console.PrintWarning(
                "tdxd: can't reach tdxd at %s or no 3D mouse connected; the keys keep "
                "their usual functions (FreeCAD as a Flatpak: run tdx-freecad-addon "
                "install, then restart FreeCAD)\n" % SOCKET)
        params = "User parameter:BaseApp/Spaceball/Buttons/%d"
        for key, num in self.numbers.items():
            if not any(key in b for b in self.data.get("banks", {}).values()):
                continue
            self.Gui.addCommand(PREFIX + key, _Key(self, key))
            p = self.FreeCAD.ParamGet(params % num)
            current = p.GetString("Command")
            if not current or current.startswith(PREFIX):  # keep the user's own choice
                p.SetString("Command", PREFIX + key)
        mw = self.Gui.getMainWindow()
        try:
            mw.workbenchActivated.connect(self.workbench_changed)
        except (AttributeError, TypeError, RuntimeError):
            # no signal from Python on this FreeCAD: look once a second
            self.timer = self.QtCore.QTimer(mw)
            self.timer.timeout.connect(self.poll)
            self.timer.start(1000)
        self.workbench_changed(self.active_workbench())
        if self.numbers:
            self.FreeCAD.Console.PrintMessage(
                "tdxd: 3D mouse keys follow the workbench (%d key sets)\n"
                % len(self.data.get("banks", {})))
        app = self.QtGui.QApplication.instance()
        if app:
            app.aboutToQuit.connect(self.stop)

    def active_workbench(self):
        wb = self.Gui.activeWorkbench()
        if not wb:
            return ""
        try:
            return wb.name()  # "PartDesignWorkbench", as FreeCAD_Navlib.xml names them
        except (AttributeError, TypeError):
            return wb.__class__.__name__

    def poll(self):
        name = self.active_workbench()
        if name != self.workbench:
            self.workbench_changed(name)

    def workbench_changed(self, name):
        self.workbench = str(name or "")
        names = key_names(self.data, self.workbench, self.numbers.keys())
        self.log("%s: %s" % (self.workbench, tdxd(app_buttons_message(0, names)).strip()))

    def stop(self):
        tdxd(app_buttons_message(0, []))

    def press(self, key):
        what = resolve(self.data, self.workbench, key)
        if not what:
            return
        if what[0] == "command":
            self.run(what[1], what[2])
        else:
            self.popup(what[1], what[2])

    def run(self, command, index):
        if command not in self.Gui.listCommands():
            self.FreeCAD.Console.PrintWarning("tdxd: FreeCAD has no command %s\n" % command)
            return
        try:
            if index is None:
                self.Gui.runCommand(command)
            else:
                self.Gui.runCommand(command, int(index))
        except Exception as e:  # noqa: BLE001 - never break FreeCAD over a key
            self.FreeCAD.Console.PrintWarning("tdxd: %s: %s\n" % (command, e))

    def popup(self, title, items):
        """3DxWare's radial menu, as a pop-up menu at the mouse pointer."""
        menu = self.QtGui.QMenu(self.Gui.getMainWindow())
        menu.setTitle(title)
        head = menu.addAction(title)
        head.setEnabled(False)
        menu.addSeparator()
        known = set(self.Gui.listCommands())
        for name, command, index in items:
            act = menu.addAction(name)
            act.setEnabled(command in known)
            act.triggered.connect(lambda _c=False, c=command, i=index: self.run(c, i))
        menu.exec_(self.QtGui.QCursor.pos())


def start():
    """Called by the add-on's InitGui.py once FreeCAD's GUI is up."""
    import FreeCAD
    import FreeCADGui as Gui
    from PySide import QtCore, QtGui
    addon = Addon(Gui, FreeCAD, QtGui, QtCore)
    QtCore.QTimer.singleShot(0, addon.start)
    return addon
