/*
 * Per-application profiles (the Linux counterpart of 3DxWare's per-application
 * configurations, e.g. Blender.xml / SolidWorks.xml in the installer).
 *
 * A profile is <profile_dir>/<name>.conf: any 3dxlinux.conf options (except the
 * daemon-wide ones) layered on top of /etc/3dxlinux.conf, plus
 *   match = blender, blender-*        process names (case-insensitive globs)
 * which defaults to <name>. The active application comes from tdx-focus /
 * "tdxctl focus APP", or else from the spacenavd client that connected last.
 *
 * Profiles come from several folders, the first match wins: profile_dir
 * (/etc/3dxlinux.d, yours), then the ones imported from 3DxWare's application
 * configurations (tdx-extract-3dxware-ui: .../3dxware-ui/profiles), system
 * wide and in the desktop user's own folder. A name already taken by an
 * earlier folder is skipped there.
 */
#ifndef TDX_PROFILE_H
#define TDX_PROFILE_H

#include "config.h"

#define TDX_MAX_PROFILES 512 /* 3DxWare has ~280 applications */

struct tdx_profile {
	char name[64];     /* file name without .conf */
	char path[512];
	char match[256];   /* from "match =", or the name */
	int user;          /* 1: imported into a user's folder (tdx_config_load_user_profile);
			    * 2: the user's own (~/.config/3dxlinux/profiles, ..._own_profile) */
};

struct tdx_profiles {
	struct tdx_profile p[TDX_MAX_PROFILES];
	int n;
};

/* Scan @dir for *.conf (sorted by name). Missing directory = no profiles. */
int tdx_profiles_scan(const char *dir, struct tdx_profiles *ps);
/* Add @dir's profiles after those already in @ps (@user: a user's folder,
 * only plain files, read as tdx_config_load_user_profile). */
int tdx_profiles_add(const char *dir, struct tdx_profiles *ps, int user);
/* Index of the first profile whose match list matches @app, or -1. */
int tdx_profile_match(const struct tdx_profiles *ps, const char *app);
/* Effective configuration: defaults, then @base_path, then profile @idx (-1 = none). */
int tdx_config_compose(struct tdx_config *out, const char *base_path,
		       const struct tdx_profiles *ps, int idx);
/* Every key code used by the base config or any profile (for uinput capabilities). */
int tdx_profiles_key_union(const char *base_path, const struct tdx_profiles *ps, int *codes, int max);
/* Application name of a process: basename of /proc/PID/exe (for an interpreter
 * such as python3, its script's name), else /proc/PID/comm. */
int tdx_app_from_pid(int pid, char *out, size_t len);
/* tdxd's own windows (settings panel, LCD Manager, applet dialogs): focusing
 * them keeps the application the device follows, as 3DxWare does for its own
 * panel. */
int tdx_is_own_app(const char *app);

#endif
