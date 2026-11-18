# SPDX-License-Identifier: MIT
# tdxd: the SpacePilot's keys follow FreeCAD's active workbench (as 3DxWare does
# on Windows). This loader is put in your FreeCAD add-on folder by
# tdx-freecad-addon; the add-on itself comes with tdxd, so upgrades update it.
import os as _os
import sys as _sys

# (/run/host/usr: the host's /usr, as FreeCAD in a Flatpak sees it)
for _d in ("/usr/share/tdxd/freecad", "/usr/local/share/tdxd/freecad",
           "/run/host/usr/share/tdxd/freecad", "/run/host/usr/local/share/tdxd/freecad"):
    if _os.path.isfile(_os.path.join(_d, "tdxd_freecad.py")):
        if _d not in _sys.path:
            _sys.path.insert(0, _d)
        break
try:
    import tdxd_freecad as _tdxd_freecad
    _tdxd_addon = _tdxd_freecad.start()
except Exception as _e:  # noqa: BLE001 - FreeCAD must start anyway
    import FreeCAD as _FreeCAD
    _FreeCAD.Console.PrintWarning("tdxd add-on not started: %s\n" % _e)
