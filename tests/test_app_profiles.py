#!/usr/bin/env python3
"""Tests for the importer's application profiles: 3DxWare's per-application
configurations (cut-down stand-ins for Blender.xml, en-US.xml ...) -> 3dxlinuxd
profile text.

    python3 tests/test_app_profiles.py
"""
import importlib.util
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what)


HEAD = '<?xml version="1.0" encoding="utf-8"?>\n<AppCfg xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">\n'


def app(cfg_id, parent, exes, actions, macros, buttons, name="STR_APP"):
    x = HEAD + "<CfgProperties><ID>%s</ID><Name>%s</Name><InheritsFromID>%s</InheritsFromID></CfgProperties>\n" % (
        cfg_id, name, parent)
    x += "<AppInfo><Signature>" + "".join("<ExecutableName>%s</ExecutableName>" % e for e in exes)
    x += "</Signature></AppInfo>\n<MacroTable>" + macros + "</MacroTable>\n<ButtonActions>"
    x += "".join('<ButtonAction Type="Macro"><ID>%s</ID><Name>%s</Name></ButtonAction>' % a for a in actions)
    x += "</ButtonActions>\n<Devices><Device><ID>ID_Standard_3D_Mouse</ID><ButtonBank Default=\"true\"><ID>Default</ID>"
    x += "".join("<Button><Input><ActionID>%s</ActionID></Input><Output><ActionID>%s</ActionID></Output></Button>"
                 % b for b in buttons)
    x += "</ButtonBank></Device>"
    # the keyboard's bank must not leak into the 3D mouse's keys
    x += "<Device><ID>ID_Standard_Keyboard</ID><ButtonBank><ID>Default</ID><Button><Input><ActionID>V3DK_ESC</ActionID>"
    x += "</Input><Output><ActionID>Macro_Top</ActionID></Output></Button></ButtonBank></Device></Devices>\n</AppCfg>\n"
    return x


EN_US = """<?xml version="1.0" encoding="UTF-8"?>
<LocalizationTables><StringTable>
<StringEntry><Key>STR_APP</Key><Value>Blendy</Value></StringEntry>
<StringEntry><Key>STR_TOP</Key><Value>Top View</Value></StringEntry>
<StringEntry><Key>STR_LONG</Key><Value>A name much longer than the LCD can show</Value></StringEntry>
</StringTable><MacroTable>
<MacroEntry><ID>Macro_Top</ID><Sequence><KeyPress>E1</KeyPress><KeyPress>5F</KeyPress>
 <KeyRelease>5F</KeyRelease><KeyRelease>E1</KeyRelease></Sequence></MacroEntry>
</MacroTable></LocalizationTables>
"""

MACROS = """
<MacroEntry><ID>Quad</ID><KeyStroke><Modifiers><Modifier>Control</Modifier><Modifier>Alt</Modifier></Modifiers><Key>14</Key></KeyStroke></MacroEntry>
<MacroEntry><ID>Menu</ID><Sequence><KeyPress>E2</KeyPress><KeyPress>19</KeyPress><KeyRelease>19</KeyRelease>
 <KeyRelease>E2</KeyRelease><Pause>100</Pause><KeyPress>17</KeyPress><KeyRelease>17</KeyRelease></Sequence></MacroEntry>
<MacroEntry><ID>Typed</ID><Sequence><Text>_Ab1</Text><ActionID>Quad</ActionID></Sequence></MacroEntry>
<MacroEntry><ID>Click</ID><Sequence><MouseButtonPress>1</MouseButtonPress><MouseButtonRelease>1</MouseButtonRelease></Sequence></MacroEntry>
<MacroEntry><ID>Weird</ID><KeyStroke><Key>ZZ</Key></KeyStroke></MacroEntry>
"""


def main():
    ex = importlib.util.spec_from_file_location("ex", os.path.join(ROOT, "tools", "extract_3dxware_ui.py"))
    m = importlib.util.module_from_spec(ex)
    ex.loader.exec_module(m)
    d = tempfile.mkdtemp(prefix="tdx-apps-")
    files = {
        "en-US.xml": EN_US,
        "AppDefCfg.xml": app("ID_Default_Cfg", "", ["Default\\.exe"],
                             [("Parent", "Parent Macro")],
                             "<MacroEntry><ID>Parent</ID><KeyStroke><Key>0A</Key></KeyStroke></MacroEntry>",
                             [("V3DK_1", "Parent")]),
        "Blender.xml": app("ID_BLENDY", "ID_Default_Cfg", ["BLENDER\\.exe", "BLENDER-APP\\.exe"],
                           [("Macro_Top", "STR_TOP"), ("Quad", "Quad View"), ("Menu", "STR_LONG"),
                            ("Typed", "Typed"), ("Parent", "Mine")],
                           MACROS,
                           [("V3DK_TOP", "Macro_Top"), ("V3DK_ISO1", "Quad"), ("V3DK_2", "Menu"),
                            ("V3DK_3", "Typed"), ("V3DK_4", "Click"), ("V3DK_5", "Weird"),
                            ("V3DK_6", "Parent"), ("V3DK_7", "AppCommand_X|Y"), ("V3DK_8", "V3DCMD_VIEW_FIT"),
                            ("V3DK_9", "RadialMenu_MenuRM_1"), ("V3DK_MENU", "Driver_ShowDriverGUI"),
                            ("V3DK_DOMINANT", "Driver_ToggleDominantFilter"), ("V3DK_10", "Driver_Disabled"),
                            ("V3DK_ESC", "KB_Esc"), ("V3DK_NORMAL", "App_PassToApplication"),
                            ("V3DK_FRONT", "V3DK_TOP")]),
        "Blender_Old.xml": app("ID_OLD", "", ["blender\\.exe"], [("Quad", "Quad")], MACROS,
                               [("V3DK_ISO1", "Quad")]),
        "NavOnly.xml": app("ID_NAV", "", ["navonly\\.exe"], [], "", [("V3DK_1", "AppCommand_A|B")]),
        "Creo.xml": app("ID_CREO", "", ["java\\.exe", "x2\\.exe", "creo\\.exe"], [("Quad", "Q")], MACROS,
                        [("V3DK_1", "Quad")]),
        "Explorer.xml": app("ID_EXP", "", ["explorer\\.exe"], [("Quad", "Q")], MACROS, [("V3DK_1", "Quad")]),
        "Regex.xml": app("ID_RE", "", ["(foo|bar)\\.exe"], [("Quad", "Q")], MACROS, [("V3DK_1", "Quad")]),
    }
    by_name = {}
    for n, t in files.items():
        p = os.path.join(d, n)
        open(p, "w", encoding="utf-8").write(t)
        by_name[n] = p
    prof = m.app_profiles(by_name)
    check(set(prof) == {"3dx-blender", "3dx-creo"}, "profiles made: %s" % sorted(prof))
    text, n = prof["3dx-blender"]
    lines = set(text.splitlines())

    def has(line):
        check(line in lines, "line %r in:\n%s" % (line, text))

    has("match = blender, blender-app")
    has("key_V3DK_TOP = KEY_LEFTSHIFT + KEY_KP7")        # macro from en-US.xml
    has("lcd_label_V3DK_TOP = Top View")                 # name from the string table
    has("key_V3DK_ISO1 = KEY_LEFTCTRL + KEY_LEFTALT + KEY_Q")
    has("lcd_label_V3DK_ISO1 = Quad View")
    has("action_V3DK_ISO1 = none")
    has("key_V3DK_2 = KEY_LEFTALT + KEY_V, KEY_T")       # chord, pause, key
    has("lcd_label_V3DK_2 = A name much longer than")    # 23 characters for the LCD
    has("key_V3DK_3 = KEY_LEFTSHIFT + KEY_MINUS, KEY_LEFTSHIFT + KEY_A, KEY_B, KEY_1, "
        "KEY_LEFTCTRL + KEY_LEFTALT + KEY_Q")            # typed text, then another macro
    has("key_V3DK_6 = KEY_G")                            # the parent's macro body
    has("lcd_label_V3DK_6 = Mine")                       # the file's own name for it
    has("action_V3DK_MENU = settings")
    has("action_V3DK_DOMINANT = dominant")
    has("key_V3DK_10 = none")                            # Driver_Disabled: nothing
    has("key_V3DK_ESC = KEY_ESC")
    has("key_V3DK_NORMAL = none")                        # the application's own button
    has("key_V3DK_FRONT = KEY_LEFTSHIFT + KEY_KP7")      # "does what TOP does"
    for k in ("V3DK_4", "V3DK_5", "V3DK_7", "V3DK_8", "V3DK_9"):
        check(not any(l.startswith(("key_%s " % k, "action_%s " % k)) for l in lines),
              "%s left to 3dxlinuxd's defaults (mouse, odd key, NavLib, SDK, menu)" % k)
    check("Blendy" in text and "Blender.xml" in text, "header names the application and file")
    check(n == 7, "keys bound to keystrokes: %d" % n)  # TOP ISO1 2 3 6 ESC FRONT
    check(prof["3dx-creo"][0].count("match = creo\n") == 1, "java and two-letter names dropped")
    # the extracted profile loads in 3dxlinuxd's syntax: every key name is known
    import re
    known = set(re.findall(r"K\(KEY_(\w+)\)", open(os.path.join(ROOT, "src", "config.c")).read()))
    used = set(re.findall(r"KEY_(\w+)", text))
    check(used <= known, "key names 3dxlinuxd knows: %s" % sorted(used - known))
    print("%d/%d checks passed" % (checks - fails, checks))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
