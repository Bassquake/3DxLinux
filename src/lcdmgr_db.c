#include "lcdmgr_db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void lm_db_device_defaults(struct lm_db *db)
{
	/* LCDMon FUN_140036c4c defaults (Brightness: the SpacePilot PRO's full scale) */
	db->brightness = 100;
	db->contrast = 2;
	db->alerts = 1;
	db->algorithm = 1;
	db->delay = 8;
	db->quickswitch = 0;
	db->invert = 0;
}

void lm_db_defaults(struct lm_db *db)
{
	memset(db, 0, sizeof *db);
	db->show_tray_icon = 1;
	db->help_auto = 1;
	db->wwidth = 550;
	db->wheight = 589;
	snprintf(db->family_name, sizeof db->family_name, "3Dconnexion SpacePilot PRO");
	db->family_id = 32;     /* LGLCD_DEVICE_FAMILY_QVGA_BASIC */
	db->device_id = 110064; /* LCDMon's id for the emulated 320x240 ext. device */
	lm_db_device_defaults(db);
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = 0;
	return s;
}

/* Read one "quoted" field with \\ escapes; returns pointer after it (and a comma). */
static char *quoted(char *s, char *out, size_t len)
{
	size_t n = 0;
	s = trim(s);
	if (*s != '"') {
		out[0] = 0;
		return s;
	}
	for (s++; *s && *s != '"'; s++) {
		if (*s == '\\' && s[1])
			s++;
		if (n + 1 < len)
			out[n++] = *s;
	}
	out[n] = 0;
	if (*s == '"')
		s++;
	if (*s == ',')
		s++;
	return s;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void parse_flags(char *s, struct lm_program *p)
{
	for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
		tok = trim(tok);
		int v = atoi(tok + 1);
		switch (tok[0]) {
		case 'E': p->enabled = v != 0; break;
		case 'A': p->autostart = v != 0; break;
		case 'S': p->persistent = v != 0; break;
		case 'L': p->allow_fg = v != 0; break;
		case 'C': p->caps = v; break;
		}
	}
}

int lm_db_load(struct lm_db *db, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -errno;
	char line[1024], sect[64] = "";
	int app_sect = -1, app_dev[LM_MAX_PROG];
	for (int i = 0; i < LM_MAX_PROG; i++)
		app_dev[i] = -1;
	while (fgets(line, sizeof line, f)) {
		char *s = trim(line);
		if (!*s || *s == ';' || *s == '#')
			continue;
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (e)
				*e = 0;
			snprintf(sect, sizeof sect, "%s", s + 1);
			app_sect = !strncasecmp(sect, "Application_", 12) ? atoi(sect + 12) : -1;
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		int iv = atoi(v);
		if (!strcasecmp(sect, "General")) {
			if (!strcasecmp(k, "UIEffects")) db->ui_effects = iv;
			else if (!strcasecmp(k, "DeviceBalloons")) db->device_balloons = iv;
			else if (!strcasecmp(k, "WarningBalloons")) db->warning_balloons = iv;
			else if (!strcasecmp(k, "ShowTrayIcon")) db->show_tray_icon = iv;
			else if (!strcasecmp(k, "ShowNewDeviceInUI")) db->show_new_device = iv;
			else if (!strcasecmp(k, "ResizeControlsAutomatically")) db->resize_controls = iv;
			else if (!strcasecmp(k, "RestoreSize")) db->restore_size = iv;
			else if (!strcasecmp(k, "WWidth")) db->wwidth = iv;
			else if (!strcasecmp(k, "WHeight")) db->wheight = iv;
			else if (!strcasecmp(k, "HelpAutomatically")) db->help_auto = iv;
			else if (!strcasecmp(k, "IUAutomatically")) db->iu_auto = iv;
		} else if (!strcasecmp(sect, "DEVICE_0")) {
			if (!strcasecmp(k, "FamilyName")) snprintf(db->family_name, sizeof db->family_name, "%s", v);
			else if (!strcasecmp(k, "FamilyId")) db->family_id = iv;
			else if (!strcasecmp(k, "DeviceId")) db->device_id = iv;
			else if (!strcasecmp(k, "Brightness")) db->brightness = clampi(iv, 0, 100);
			else if (!strcasecmp(k, "Contrast")) db->contrast = clampi(iv, 0, 2);
			else if (!strcasecmp(k, "Alerts")) db->alerts = clampi(iv, 0, 1);
			else if (!strcasecmp(k, "Algorithm")) db->algorithm = clampi(iv, 1, 3);
			else if (!strcasecmp(k, "Delay")) db->delay = clampi(iv, 1, 20);
			else if (!strcasecmp(k, "QuickSwitch")) db->quickswitch = clampi(iv, 0, 1);
			else if (!strcasecmp(k, "InvertColors")) db->invert = clampi(iv, 0, 1);
			else if (!strcasecmp(k, "ManualModeApplet")) {
				char *r = quoted(v, db->manual_path, sizeof db->manual_path);
				quoted(r, db->manual_name, sizeof db->manual_name);
			}
		} else if (!strcasecmp(sect, "ClientApplications")) {
			if (db->nprog >= LM_MAX_PROG)
				continue;
			struct lm_program *p = &db->prog[db->nprog];
			memset(p, 0, sizeof *p);
			p->enabled = 1;
			p->allow_fg = 1;
			p->dev_enabled = 1;
			char *r = quoted(v, p->path, sizeof p->path);
			r = quoted(r, p->name, sizeof p->name);
			parse_flags(r, p);
			if (p->path[0] || p->name[0])
				db->nprog++;
		} else if (app_sect >= 0 && app_sect < LM_MAX_PROG && !strcmp(k, "0")) {
			/* LCDMon writes [Application_N] before [ClientApplications]: apply later */
			char *e = strstr(v, ",E");
			if (e)
				app_dev[app_sect] = atoi(e + 2) != 0;
		}
	}
	fclose(f);
	for (int i = 0; i < db->nprog; i++)
		if (app_dev[i] >= 0)
			db->prog[i].dev_enabled = app_dev[i];
	return 0;
}

static void put_quoted(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		fputc(*s, f);
	}
	fputc('"', f);
}

static int mkdirs(const char *path)
{
	char tmp[LM_PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				return -errno;
			*p = '/';
		}
	return 0;
}

int lm_db_save(const struct lm_db *db, const char *path)
{
	char tmp[LM_PATH_MAX + 8];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	mkdirs(path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -errno;
	fprintf(f, "[General]\nWWidth=%d\nHelpAutomatically=%d\nShowNewDeviceInUI=%d\nUIEffects=%d\n"
		   "WarningBalloons=%d\nDeviceBalloons=%d\nShowTrayIcon=%d\nVersion=30\n"
		   "ResizeControlsAutomatically=%d\nIUAutomatically=%d\nWHeight=%d\nRestoreSize=%d\n\n",
		db->wwidth, db->help_auto, db->show_new_device, db->ui_effects, db->warning_balloons,
		db->device_balloons, db->show_tray_icon, db->resize_controls, db->iu_auto, db->wheight,
		db->restore_size);
	fprintf(f, "[DEVICE_0]\nBrightness=%d\nFamilyId=%d\nAlerts=%d\nInvertColors=%d\nDeviceId=%d\n"
		   "ManualModeApplet=",
		db->brightness, db->family_id, db->alerts, db->invert, db->device_id);
	put_quoted(f, db->manual_path);
	fputc(',', f);
	put_quoted(f, db->manual_name);
	fprintf(f, "\nQuickSwitch=%d\nFamilyName=%s\nAlgorithm=%d\nDelay=%d\nContrast=%d\n\n",
		db->quickswitch, db->family_name, db->algorithm, db->delay, db->contrast);
	int saved = 0;
	for (int i = 0; i < db->nprog; i++) /* LCDMon keeps only persistent programs */
		if (db->prog[i].persistent)
			fprintf(f, "[Application_%d]\n0=F%d,I%d,E%d\n\n", saved++, db->family_id,
				db->device_id, db->prog[i].dev_enabled);
	fprintf(f, "[ClientApplications]\n");
	saved = 0;
	for (int i = 0; i < db->nprog; i++) {
		const struct lm_program *p = &db->prog[i];
		if (!p->persistent)
			continue;
		fprintf(f, "%d=", saved++);
		put_quoted(f, p->path);
		fputc(',', f);
		put_quoted(f, p->name);
		fprintf(f, ",E%d,A%d,S%d,L%d,C%d\n", p->enabled, p->autostart, p->persistent, p->allow_fg,
			p->caps);
	}
	fputc('\n', f);
	if (fclose(f) != 0)
		return -errno;
	return rename(tmp, path) < 0 ? -errno : 0;
}

int lm_db_find(const struct lm_db *db, const char *path, const char *name)
{
	for (int i = 0; i < db->nprog; i++)
		if (!strcmp(db->prog[i].path, path) && !strcmp(db->prog[i].name, name))
			return i;
	return -1;
}

int lm_db_add(struct lm_db *db, const char *path, const char *name, int caps, int persistent,
	      int autostartable)
{
	int i = lm_db_find(db, path, name);
	if (i >= 0) {
		db->prog[i].caps = caps;
		if (persistent)
			db->prog[i].persistent = 1;
		return i;
	}
	if (db->nprog >= LM_MAX_PROG)
		return -1;
	struct lm_program *p = &db->prog[db->nprog];
	memset(p, 0, sizeof *p);
	snprintf(p->path, sizeof p->path, "%s", path);
	snprintf(p->name, sizeof p->name, "%s", name);
	p->enabled = 1;
	p->autostart = autostartable ? 1 : 0;
	p->persistent = persistent ? 1 : 0;
	p->allow_fg = 1;
	p->caps = caps;
	p->dev_enabled = 1;
	return db->nprog++;
}

void lm_db_default_path(char *out, unsigned len)
{
	const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	char base[LM_PATH_MAX + 32], old[LM_PATH_MAX + 64];
	if (x && *x)
		snprintf(base, sizeof base, "%.200s", x);
	else
		snprintf(base, sizeof base, "%.200s/.config", h && *h ? h : "/tmp");
	snprintf(out, len, "%s/3dxlinux/lcdmgr.ini", base);
	snprintf(old, sizeof old, "%s/tdxd/lcdmgr.ini", base); /* before 0.3.5 */
	if (access(out, F_OK) && !access(old, F_OK))
		snprintf(out, len, "%s", old);
}
