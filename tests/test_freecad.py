#!/usr/bin/env python3
"""Tests for the FreeCAD key sets: the importer (FreeCAD_Navlib.xml -> buttons.json)
and the tdxd FreeCAD add-on, with stand-ins for FreeCAD and tdxd's control socket.

    python3 tests/test_freecad.py
"""
import importlib.util
import json
import os
import socket
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# the structure of 3DxWare's FreeCAD_Navlib.xml, cut down
NAVLIB = """<?xml version="1.0" encoding="utf-8"?>
<AppCfg xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
 <ButtonActions>
  <ButtonAction Type="AppCommand" xsi:type="ButtonAction_AppCommand"><ID>AppCommand_File|Std_New</ID><Name>New</Name></ButtonAction>
  <ButtonAction Type="AppCommand" xsi:type="ButtonAction_AppCommand"><ID>AppCommand_PartDesign|PartDesign_NewSketch</ID><Name>Create sketch</Name></ButtonAction>
  <ButtonAction Type="AppCommand" xsi:type="ButtonAction_AppCommand"><ID>AppCommand_PartDesign|PartDesign_CompDatums|3</ID><Name>Create a local coordinate system</Name></ButtonAction>
  <ButtonAction Type="AppCommand" xsi:type="ButtonAction_AppCommand"><ID>AppCommand_PartDesign|PartDesign_Pad</ID><Name>Pad</Name></ButtonAction>
  <ButtonAction Type="AppCommand" xsi:type="ButtonAction_AppCommand"><ID>AppCommand_PartDesign|PartDesign_Pocket</ID><Name>Pocket</Name></ButtonAction>
  <ButtonAction Type="Menu" xsi:type="ButtonAction_Menu"><ID>RadialMenu_MenuRM_1</ID><Name>Part Design Modeling</Name><MenuID>MenuRM_1</MenuID></ButtonAction>
 </ButtonActions>
 <Menu Type="Radial"><ID>MenuRM_1</ID><Name>Part Design Modeling</Name>
  <Button><Input><ActionID>MenuButton_1</ActionID></Input><Output><ActionID>AppCommand_PartDesign|PartDesign_Pad</ActionID></Output></Button>
  <Button><Input><ActionID>MenuButton_2</ActionID></Input><Output><ActionID>AppCommand_PartDesign|PartDesign_Pocket</ActionID></Output></Button>
 </Menu>
 <Devices><Device><ID>ID_Standard_3D_Mouse</ID>
  <ButtonBank Default="true"><ID>Default</ID>
   <Button><Input><ActionID>V3DK_1</ActionID></Input><Output><ActionID>AppCommand_File|Std_New</ActionID></Output></Button>
  </ButtonBank>
  <ButtonBank><ID>PartDesignWorkbench</ID>
   <Button><Input><ActionID>V3DK_1</ActionID></Input><Output><ActionID>AppCommand_PartDesign|PartDesign_NewSketch</ActionID></Output></Button>
   <Button><Input><ActionID>V3DK_2</ActionID></Input><Output><ActionID>RadialMenu_MenuRM_1</ActionID></Output></Button>
   <Button><Input><ActionID>V3DK_6</ActionID></Input><Output><ActionID>AppCommand_PartDesign|PartDesign_CompDatums|3</ActionID></Output></Button>
   <Button><Input><ActionID>V3DK_7</ActionID></Input><Output><ActionID>Driver_ToggleRotations</ActionID></Output></Button>
  </ButtonBank>
 </Device>
 <Device><ID>ID_Standard_Keyboard</ID><ButtonBank><ID>Default</ID>
   <Button><Input><ActionID>V3DK_9</ActionID></Input><Output><ActionID>AppCommand_File|Std_New</ActionID></Output></Button>
 </ButtonBank></Device></Devices>
</AppCfg>"""


class FakeTdxd:
    """tdxd's control socket: answers "buttons", records app-buttons."""

    def __init__(self, path):
        self.path, self.got = path, []
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.bind(path)
        self.s.listen(8)
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            c, _ = self.s.accept()
            line = c.recv(4096).decode().rstrip("\n")
            self.got.append(line)
            if line == "buttons":
                c.sendall(b"12 V3DK_1\n13 V3DK_2\n17 V3DK_6\n18 V3DK_7\n27 V3DK_PANZOOM\n")
            else:
                c.sendall(b"ok\n")
            c.close()


def main():
    d = tempfile.mkdtemp(prefix="tdx-freecad-")
    ex = load("ex", os.path.join(ROOT, "tools", "extract_3dxware_ui.py"))
    xml = os.path.join(d, "FreeCAD_Navlib.xml")
    open(xml, "w").write(NAVLIB)
    fc = ex.freecad_buttons(xml)
    pd = fc["banks"]["PartDesignWorkbench"]
    check(fc["banks"]["Default"] == {"V3DK_1": "AppCommand_File|Std_New"}, "default bank, 3D mouse only")
    check(set(pd) == {"V3DK_1", "V3DK_2", "V3DK_6"}, "3DxWare's own functions stay with tdxd: %s" % sorted(pd))
    check(fc["actions"][pd["V3DK_6"]] == {"name": "Create a local coordinate system",
                                          "command": "PartDesign_CompDatums", "index": 3},
          "group command entry")
    m = fc["actions"][pd["V3DK_2"]]
    check(m["name"] == "Part Design Modeling" and fc["menus"][m["menu"]]["items"] ==
          ["AppCommand_PartDesign|PartDesign_Pad", "AppCommand_PartDesign|PartDesign_Pocket"],
          "radial menu with its entries")

    # the add-on's own logic
    os.makedirs(os.path.join(d, "art", "freecad"))
    json.dump(fc, open(os.path.join(d, "art", "freecad", "buttons.json"), "w"))
    os.environ["TDX_UI_ASSETS"] = os.path.join(d, "art")
    sock = os.path.join(d, "tdxd.sock")
    os.environ["TDXD_SOCKET"] = sock
    tf = load("tdxd_freecad", os.path.join(ROOT, "packaging", "freecad", "tdxd_freecad.py"))
    data = tf.load_buttons()
    check(data is not None, "buttons.json found in the asset dirs")
    check(tf.bank_for(data, "StartWorkbench") == data["banks"]["Default"], "no set: the default one")
    check(tf.resolve(data, "PartDesignWorkbench", "V3DK_6") == ("command", "PartDesign_CompDatums", 3),
          "resolve a group command entry")
    check(tf.resolve(data, "PartDesignWorkbench", "V3DK_2") ==
          ("menu", "Part Design Modeling", [("Pad", "PartDesign_Pad", None),
                                             ("Pocket", "PartDesign_Pocket", None)]), "resolve a menu")
    check(tf.resolve(data, "PartDesignWorkbench", "V3DK_7") is None, "a key the set leaves out")
    msg = tf.app_buttons_message(123, tf.key_names(data, "PartDesignWorkbench", ["V3DK_1", "V3DK_7"]))
    check(msg == "app-buttons\t123\tV3DK_1\tCreate sketch", "app-buttons message: %r" % msg)
    check(tf.parse_buttons("12 V3DK_1\n27 V3DK_PANZOOM\nerror x\n") == {"V3DK_1": 12, "V3DK_PANZOOM": 27},
          "parse tdxctl buttons")

    # the add-on in a stand-in FreeCAD
    srv = FakeTdxd(sock)
    params, commands, ran, menus = {}, {}, [], []

    class Param:
        def __init__(self, path):
            self.path = path

        def GetString(self, k):
            return params.get((self.path, k), "")

        def SetString(self, k, v):
            params[(self.path, k)] = v

    class FakeConsole:
        PrintLog = PrintMessage = PrintWarning = staticmethod(lambda t: None)

    class FreeCAD:
        ParamGet = staticmethod(Param)
        Console = FakeConsole

    class Signal:
        def __init__(self):
            self.f = None

        def connect(self, f):
            self.f = f

    class Workbench:
        def __init__(self, n):
            self.n = n

        def name(self):
            return self.n

    class MainWindow:
        workbenchActivated = Signal()

    mw = MainWindow()

    class Gui:
        wb = Workbench("PartDesignWorkbench")
        addCommand = staticmethod(lambda n, c: commands.__setitem__(n, c))
        getMainWindow = staticmethod(lambda: mw)
        activeWorkbench = staticmethod(lambda: Gui.wb)
        listCommands = staticmethod(lambda: ["PartDesign_NewSketch", "PartDesign_CompDatums",
                                             "PartDesign_Pad", "Std_New"])

        @staticmethod
        def runCommand(name, index=None):
            ran.append((name, index))

    class Action:
        def __init__(self, text):
            self.text, self.enabled, self.slot = text, True, None
            self.triggered = self

        def setEnabled(self, e):
            self.enabled = e

        def connect(self, f):
            self.slot = f

    class FakeMenu:
        def __init__(self, parent):
            self.items = []
            menus.append(self)

        def setTitle(self, t):
            self.title = t

        def addAction(self, t):
            a = Action(t)
            self.items.append(a)
            return a

        def addSeparator(self):
            pass

        def exec_(self, pos):
            self.items[1].slot()  # choose "Pad" (0 is the title)

    class QtGui:
        QMenu = FakeMenu

        class QCursor:
            pos = staticmethod(lambda: (0, 0))

        class QApplication:
            instance = staticmethod(lambda: None)

    params[("User parameter:BaseApp/Spaceball/Buttons/13", "Command")] = "Std_ViewFitAll"  # the user's own
    addon = tf.Addon(Gui, FreeCAD, QtGui, None)
    addon.start()
    check(set(commands) == {"Tdxd_Key_V3DK_1", "Tdxd_Key_V3DK_2", "Tdxd_Key_V3DK_6"},
          "one FreeCAD command per key in a set: %s" % sorted(commands))
    check(params.get(("User parameter:BaseApp/Spaceball/Buttons/12", "Command")) == "Tdxd_Key_V3DK_1",
          "FreeCAD's button map points at the key command")
    check(params.get(("User parameter:BaseApp/Spaceball/Buttons/13", "Command")) == "Std_ViewFitAll",
          "the user's own button command is kept")
    check(srv.got[-1] == "app-buttons\t%d\tV3DK_1\tCreate sketch\tV3DK_2\tPart Design Modeling\t"
          "V3DK_6\tCreate a local coordinate system" % 0, "names sent: %r" % srv.got[-1])
    commands["Tdxd_Key_V3DK_6"].Activated()
    check(ran[-1] == ("PartDesign_CompDatums", 3), "key 6 runs the group command's entry 3")
    commands["Tdxd_Key_V3DK_2"].Activated()
    check(menus and menus[-1].title == "Part Design Modeling" and ran[-1] == ("PartDesign_Pad", None),
          "key 2 pops up the radial menu, its entry runs")
    check(menus[-1].items[2].enabled is False, "an entry FreeCAD lacks is greyed out (Pocket)")
    mw.workbenchActivated.f("SketcherWorkbench")  # no set: the default one
    check(srv.got[-1] == "app-buttons\t%d\tV3DK_1\tNew" % 0, "workbench change: %r" % srv.got[-1])
    commands["Tdxd_Key_V3DK_1"].Activated()
    check(ran[-1] == ("Std_New", None), "the default set's command")
    addon.stop()
    check(srv.got[-1] == "app-buttons\t%d" % 0, "on exit the keys go back to tdxd")

    print("%d/%d checks passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
