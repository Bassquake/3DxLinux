#define _GNU_SOURCE
#include "profile.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int cmp_profile(const void *a, const void *b)
{
	return strcmp(((const struct tdx_profile *)a)->name, ((const struct tdx_profile *)b)->name);
}

int tdx_profiles_scan(const char *dir, struct tdx_profiles *ps)
{
	ps->n = 0;
	DIR *d = opendir(dir);
	if (!d)
		return errno == ENOENT ? 0 : -errno;
	struct dirent *e;
	while ((e = readdir(d)) && ps->n < TDX_MAX_PROFILES) {
		size_t len = strlen(e->d_name);
		if (e->d_name[0] == '.' || len < 6 || strcmp(e->d_name + len - 5, ".conf"))
			continue;
		struct tdx_profile *p = &ps->p[ps->n];
		snprintf(p->name, sizeof p->name, "%.*s", (int)(len - 5), e->d_name);
		snprintf(p->path, sizeof p->path, "%s/%s", dir, e->d_name);
		/* read only the match line; full parsing happens when the profile is applied */
		char match[256] = "";
		FILE *f = fopen(p->path, "r");
		if (f) {
			char line[512];
			while (fgets(line, sizeof line, f)) {
				char *s = line;
				while (isspace((unsigned char)*s))
					s++;
				if (!strncmp(s, "match", 5)) {
					char *eq = strchr(s, '=');
					if (eq) {
						char *v = eq + 1;
						while (isspace((unsigned char)*v))
							v++;
						v[strcspn(v, "#\r\n")] = 0;
						size_t vl = strlen(v);
						while (vl && isspace((unsigned char)v[vl - 1]))
							v[--vl] = 0;
						snprintf(match, sizeof match, "%s", v);
					}
				}
			}
			fclose(f);
		}
		snprintf(p->match, sizeof p->match, "%s", match[0] ? match : p->name);
		ps->n++;
	}
	closedir(d);
	qsort(ps->p, (size_t)ps->n, sizeof ps->p[0], cmp_profile);
	return 0;
}

int tdx_profile_match(const struct tdx_profiles *ps, const char *app)
{
	if (!app || !*app)
		return -1;
	for (int i = 0; i < ps->n; i++) {
		char buf[256];
		snprintf(buf, sizeof buf, "%s", ps->p[i].match);
		for (char *tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t"))
			if (!fnmatch(tok, app, FNM_CASEFOLD))
				return i;
	}
	return -1;
}

int tdx_config_compose(struct tdx_config *out, const char *base_path,
		       const struct tdx_profiles *ps, int idx)
{
	tdx_config_defaults(out);
	int rc = tdx_config_load(out, base_path);
	if (idx >= 0 && idx < ps->n)
		tdx_config_load_profile(out, ps->p[idx].path);
	out->match[0] = 0;
	return rc;
}

static void add_code(int *codes, int *n, int max, int code)
{
	for (int i = 0; i < *n; i++)
		if (codes[i] == code)
			return;
	if (*n < max)
		codes[(*n)++] = code;
}

int tdx_profiles_key_union(const char *base_path, const struct tdx_profiles *ps, int *codes, int max)
{
	int n = 0;
	struct tdx_config *c = malloc(sizeof *c);
	if (!c)
		return 0;
	for (int idx = -1; idx < ps->n; idx++) {
		tdx_config_compose(c, base_path, ps, idx);
		for (int i = 0; i < c->nkeymap; i++)
			for (int k = 0; k < 4 && c->keymap[i].keys[k]; k++)
				add_code(codes, &n, max, c->keymap[i].keys[k]);
	}
	free(c);
	return n;
}

/* An interpreter whose program is its script: python3.14, perl, node, sh ... */
static int is_interpreter(const char *exe)
{
	static const char *const pre[] = {"python", "perl", "ruby", "node", "bash", "dash", "sh", NULL};
	for (int i = 0; pre[i]; i++) {
		size_t n = strlen(pre[i]);
		if (!strncmp(exe, pre[i], n) && (!exe[n] || exe[n] == '.' || isdigit((unsigned char)exe[n])))
			return 1;
	}
	return 0;
}

/* The script an interpreter runs (/proc/<pid>/cmdline): "python3 -B /usr/bin/tdx-settings"
 * -> "tdx-settings", "python3 -m foo.bar" -> "foo.bar". 0, or -1 if none. */
static int script_of(int pid, char *out, size_t len)
{
	char p[64], buf[4096];
	snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
	FILE *f = fopen(p, "r");
	if (!f)
		return -1;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	const char *a = buf, *end = buf + n;
	a += strlen(a) + 1; /* argv[0], the interpreter */
	while (a < end && *a) {
		if (!strcmp(a, "-c"))
			return -1;            /* a one-liner: no script */
		if (!strcmp(a, "-m")) {       /* python -m module */
			a += strlen(a) + 1;
			if (a >= end || !*a)
				return -1;
			snprintf(out, len, "%s", a);
			return 0;
		}
		if (*a != '-') {
			const char *base = strrchr(a, '/');
			snprintf(out, len, "%s", base ? base + 1 : a);
			return 0;
		}
		a += strlen(a) + 1;
	}
	return -1;
}

int tdx_app_from_pid(int pid, char *out, size_t len)
{
	char p[64], buf[512];
	snprintf(p, sizeof p, "/proc/%d/exe", pid);
	ssize_t n = readlink(p, buf, sizeof buf - 1);
	if (n > 0) {
		buf[n] = 0;
		char *del = strstr(buf, " (deleted)");
		if (del)
			*del = 0;
		const char *base = strrchr(buf, '/');
		base = base ? base + 1 : buf;
		/* a script is the application, not its interpreter (tdx-settings runs
		 * as /usr/bin/python3.14) */
		if (is_interpreter(base) && script_of(pid, out, len) == 0)
			return 0;
		snprintf(out, len, "%s", base);
		return 0;
	}
	snprintf(p, sizeof p, "/proc/%d/comm", pid);
	FILE *f = fopen(p, "r");
	if (!f)
		return -errno;
	int ok = fgets(buf, sizeof buf, f) != NULL;
	fclose(f);
	if (!ok)
		return -EIO;
	buf[strcspn(buf, "\n")] = 0;
	snprintf(out, len, "%s", buf);
	return 0;
}

int tdx_is_own_app(const char *app)
{
	static const char *const own[] = {"tdx-settings", "tdx-settings-helper", "tdx-lcdmanager",
					  "tdx-lcd-functionkeys", "tdx-lcd-rss", "tdx-lcd-pictures",
					  "tdx-lcd-demo", "tdx-extract-3dxware-ui", NULL};
	for (int i = 0; app && own[i]; i++)
		if (!strcasecmp(app, own[i]))
			return 1;
	return 0;
}
