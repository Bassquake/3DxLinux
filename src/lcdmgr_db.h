/*
 * tdx-lcdmgr settings: the LCD Manager database, same layout and keys as
 * LCDMon's %LOCALAPPDATA%\Logitech\LCD Software\LCD Manager\LGLCDDB.TXT
 * (LCDMon.exe FUN_140032680 writes it, FUN_140034f0c / FUN_140036c4c read it):
 *
 *   [General]   UIEffects DeviceBalloons WarningBalloons ShowTrayIcon
 *               ShowNewDeviceInUI ResizeControlsAutomatically RestoreSize
 *               WWidth WHeight HelpAutomatically IUAutomatically Version=30
 *   [DEVICE_0]  FamilyName FamilyId DeviceId Brightness Contrast(0..2)
 *               Alerts(0..1, def 1) Algorithm(1..3, def 1) Delay(1..20, def 8)
 *               QuickSwitch(def 0) InvertColors(def 0) ManualModeApplet="path","name"
 *   [ClientApplications]  N="path","name",E1,A1,S1,L1,C2
 *               E enabled (def 1)  A autostart (def 0)  S persistent (def 0)
 *               L allow force-to-foreground (def 1)  C applet capabilities
 *   [Application_N]  0=F<family>,I<device id>,E<enabled on that device>
 *
 * Algorithm: 1 manual (device controls), 2 rotate every Delay s, 3 Klever-VU.
 * Stored at $XDG_CONFIG_HOME/3dxlinux/lcdmgr.ini (default ~/.config/3dxlinux/lcdmgr.ini).
 */
#ifndef TDX_LCDMGR_DB_H
#define TDX_LCDMGR_DB_H

#include "lcdmgr_proto.h"

#define LM_MAX_PROG 32
#define LM_PATH_MAX 256

struct lm_program {
	char path[LM_PATH_MAX];
	char name[LM_NAME_MAX];
	int enabled, autostart, persistent, allow_fg, caps;
	int dev_enabled; /* [Application_N] E for DEVICE_0 */
};

struct lm_db {
	/* [General] */
	int ui_effects, device_balloons, warning_balloons, show_tray_icon, show_new_device,
		resize_controls, restore_size, wwidth, wheight, help_auto, iu_auto;
	/* [DEVICE_0] */
	char family_name[64];
	int family_id, device_id;
	int brightness, contrast, alerts, algorithm, delay, quickswitch, invert;
	char manual_path[LM_PATH_MAX], manual_name[LM_NAME_MAX];
	/* [ClientApplications] */
	struct lm_program prog[LM_MAX_PROG];
	int nprog;
};

void lm_db_defaults(struct lm_db *db);
/* Device settings only (the LCD Manager "Defaults" button). */
void lm_db_device_defaults(struct lm_db *db);
int lm_db_load(struct lm_db *db, const char *path); /* 0 or -errno (defaults kept) */
int lm_db_save(const struct lm_db *db, const char *path);
int lm_db_find(const struct lm_db *db, const char *path, const char *name); /* index or -1 */
/* Find or append a program; returns its index or -1 if full. */
int lm_db_add(struct lm_db *db, const char *path, const char *name, int caps, int persistent,
	      int autostartable);
/* Default database path into @out. */
void lm_db_default_path(char *out, unsigned len);

#endif
