# SPDX-License-Identifier: MIT
# 3dxlinuxd: the SpacePilot's keys follow FreeCAD's active workbench (as 3DxWare does
# on Windows). This loader is put in your FreeCAD add-on folder by
# 3dxlinux-freecad-addon; the add-on itself comes with 3dxlinuxd, so upgrades update it.
import os as _os
import sys as _sys

# (/run/host/usr: the host's /usr, as FreeCAD in a Flatpak sees it)
for _d in ("/usr/share/3dxlinux/freecad", "/usr/local/share/3dxlinux/freecad",
           "/run/host/usr/share/3dxlinux/freecad", "/run/host/usr/local/share/3dxlinux/freecad"):
    if _os.path.isfile(_os.path.join(_d, "freecad_3dxlinux.py")):
        if _d not in _sys.path:
            _sys.path.insert(0, _d)
        break
try:
    import freecad_3dxlinux as _freecad_3dxlinux
    _tdxd_addon = _freecad_3dxlinux.start()
except Exception as _e:  # noqa: BLE001 - FreeCAD must start anyway
    import FreeCAD as _FreeCAD
    _FreeCAD.Console.PrintWarning("3dxlinuxd add-on not started: %s\n" % _e)
