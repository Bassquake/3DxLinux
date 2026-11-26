/* lib3dxlinuxlcd: lglcd-compatible client for 3dxlinux-lcdmgr (see 3dxlinuxlcd.h, lcdmgr_proto.h). */
#define _GNU_SOURCE
#include "3dxlinuxlcd.h"
#include "lcdmgr_proto.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CONN 16
#define MAX_DEV 8

struct dev {
	int used;
	uint32_t srv;  /* server device id */
	lgLcdSoftbuttonsChangedContext sb;
	DWORD buttons;
};

struct conn {
	int used, fd, dead;
	pthread_t th;
	pthread_mutex_t req; /* one request in flight */
	pthread_mutex_t m;
	pthread_cond_t cv;
	int have_reply;
	uint32_t rstatus;
	uint8_t rdata[256];
	uint32_t rlen;
	lgLcdConfigureContext cfg;
	lgLcdNotificationContext ntf;
	struct dev dev[MAX_DEV];
};

static struct conn conns[MAX_CONN];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_init;
static char g_sock[108];

void tdxLcdSetSocketPath(const char *path)
{
	snprintf(g_sock, sizeof g_sock, "%s", path ? path : "");
}

static void default_sock(char *out, size_t n)
{
	const char *env = getenv("DXLINUX_LCD_SOCKET");
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (g_sock[0])
		snprintf(out, n, "%s", g_sock);
	else if (env && *env)
		snprintf(out, n, "%s", env);
	else if (rt && *rt)
		snprintf(out, n, "%s/3dxlinux-lcd.sock", rt);
	else
		snprintf(out, n, "/tmp/3dxlinux-lcd-%u.sock", (unsigned)getuid());
}

static int read_full(int fd, void *buf, size_t len)
{
	for (size_t o = 0; o < len;) {
		ssize_t r = read(fd, (char *)buf + o, len - o);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		o += (size_t)r;
	}
	return 0;
}

static int write_iov(int fd, struct iovec *iov, int n)
{
	while (n > 0) {
		ssize_t w = writev(fd, iov, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		while (n > 0 && (size_t)w >= iov->iov_len) {
			w -= (ssize_t)iov->iov_len;
			iov++, n--;
		}
		if (n > 0) {
			iov->iov_base = (char *)iov->iov_base + w;
			iov->iov_len -= (size_t)w;
		}
	}
	return 0;
}

static struct dev *dev_by_srv(struct conn *c, uint32_t srv)
{
	for (int i = 0; i < MAX_DEV; i++)
		if (c->dev[i].used && c->dev[i].srv == srv)
			return &c->dev[i];
	return NULL;
}

static void *reader(void *arg)
{
	struct conn *c = arg;
	int ci = (int)(c - conns);
	uint8_t buf[256];
	for (;;) {
		struct lm_hdr h;
		if (read_full(c->fd, &h, sizeof h))
			break;
		if (h.len > sizeof buf) { /* never sent by the server; skip */
			uint8_t junk[512];
			size_t left = h.len;
			while (left) {
				size_t k = left < sizeof junk ? left : sizeof junk;
				if (read_full(c->fd, junk, k))
					goto out;
				left -= k;
			}
			continue;
		}
		if (h.len && read_full(c->fd, buf, h.len))
			break;
		uint32_t *u = (uint32_t *)buf;
		if (h.type == LM_REPLY && h.len >= 8) {
			pthread_mutex_lock(&c->m);
			c->rstatus = u[1];
			c->rlen = h.len - 8;
			memcpy(c->rdata, buf + 8, c->rlen);
			c->have_reply = 1;
			pthread_cond_broadcast(&c->cv);
			pthread_mutex_unlock(&c->m);
		} else if (h.type == LM_EV_BUTTONS && h.len >= 8) {
			struct dev *d = dev_by_srv(c, u[0]);
			if (d) {
				d->buttons = u[1];
				if (d->sb.softbuttonsChangedCallback)
					d->sb.softbuttonsChangedCallback(ci << 8 | (int)(d - c->dev), u[1],
									 d->sb.softbuttonsChangedContext);
			}
		} else if (h.type == LM_EV_CONFIGURE) {
			if (c->cfg.configCallback)
				c->cfg.configCallback(ci, c->cfg.configContext);
		} else if (h.type == LM_EV_NOTIFY && h.len >= 20) {
			if (c->ntf.notificationCallback)
				c->ntf.notificationCallback(ci, c->ntf.notifyContext, u[0], u[1], u[2], u[3],
							    u[4]);
		}
	}
out:
	pthread_mutex_lock(&c->m);
	c->dead = 1;
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->m);
	if (c->ntf.notificationCallback)
		c->ntf.notificationCallback(ci, c->ntf.notifyContext,
					    LGLCD_NOTIFICATION_CLOSE_CONNECTION, 0, 0, 0, 0);
	return NULL;
}

/* Send a request and wait for its reply. data/len: optional second payload part. */
static DWORD request(struct conn *c, uint32_t type, const void *p1, uint32_t n1, const void *p2,
		     uint32_t n2, void *out, uint32_t outlen)
{
	pthread_mutex_lock(&c->req);
	pthread_mutex_lock(&c->m);
	int dead = c->dead;
	c->have_reply = 0;
	pthread_mutex_unlock(&c->m);
	if (dead) {
		pthread_mutex_unlock(&c->req);
		return LM_ERROR_SERVICE_NOT_ACTIVE;
	}
	struct lm_hdr h = {type, n1 + n2};
	struct iovec iov[3] = {{&h, sizeof h}, {(void *)p1, n1}, {(void *)p2, n2}};
	if (write_iov(c->fd, iov, 3)) {
		pthread_mutex_unlock(&c->req);
		return LM_ERROR_SERVICE_NOT_ACTIVE;
	}
	pthread_mutex_lock(&c->m);
	while (!c->have_reply && !c->dead)
		pthread_cond_wait(&c->cv, &c->m);
	DWORD st = c->have_reply ? c->rstatus : LM_ERROR_SERVICE_NOT_ACTIVE;
	if (c->have_reply && out)
		memcpy(out, c->rdata, c->rlen < outlen ? c->rlen : outlen);
	pthread_mutex_unlock(&c->m);
	pthread_mutex_unlock(&c->req);
	return st;
}

static struct conn *conn_get(int connection)
{
	if (connection < 0 || connection >= MAX_CONN || !conns[connection].used)
		return NULL;
	return &conns[connection];
}

static struct conn *dev_get(int device, struct dev **d)
{
	struct conn *c = conn_get(device >> 8);
	int di = device & 0xFF;
	if (!c || di >= MAX_DEV || !c->dev[di].used)
		return NULL;
	*d = &c->dev[di];
	return c;
}

DWORD lgLcdInit(void)
{
	pthread_mutex_lock(&g_lock);
	int was = g_init;
	g_init = 1;
	pthread_mutex_unlock(&g_lock);
	return was ? LM_ERROR_ALREADY_INITIALIZED : LM_OK;
}

DWORD lgLcdDeInit(void)
{
	for (int i = 0; i < MAX_CONN; i++)
		if (conns[i].used)
			lgLcdDisconnect(i);
	g_init = 0;
	return LM_OK;
}

DWORD lgLcdConnectEx(lgLcdConnectContextEx *ctx)
{
	if (!ctx || !ctx->appFriendlyName)
		return LM_ERROR_INVALID_PARAMETER;
	if (!g_init)
		return LM_ERROR_SERVICE_NOT_ACTIVE;
	pthread_mutex_lock(&g_lock);
	int ci = -1;
	for (int i = 0; i < MAX_CONN; i++)
		if (!conns[i].used) {
			ci = i;
			break;
		}
	if (ci < 0) {
		pthread_mutex_unlock(&g_lock);
		return LM_ERROR_NO_MORE_ITEMS;
	}
	struct conn *c = &conns[ci];
	memset(c, 0, sizeof *c);
	c->used = 1;
	pthread_mutex_unlock(&g_lock);

	struct sockaddr_un a = {.sun_family = AF_UNIX};
	default_sock(a.sun_path, sizeof a.sun_path);
	c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (c->fd < 0 || connect(c->fd, (struct sockaddr *)&a, sizeof a) < 0) {
		if (c->fd >= 0)
			close(c->fd);
		c->used = 0;
		return LM_ERROR_FILE_NOT_FOUND; /* LCD Manager not running */
	}
	pthread_mutex_init(&c->req, NULL);
	pthread_mutex_init(&c->m, NULL);
	pthread_cond_init(&c->cv, NULL);
	c->cfg = ctx->onConfigure;
	c->ntf = ctx->onNotify;
	if (pthread_create(&c->th, NULL, reader, c)) {
		close(c->fd);
		c->used = 0;
		return LM_ERROR_SERVICE_NOT_ACTIVE;
	}
	uint32_t hdr[3] = {LM_VERSION, ctx->dwAppletCapabilitiesSupported,
			   (ctx->isPersistent ? LM_CF_PERSISTENT : 0) |
				   (ctx->isAutostartable ? LM_CF_AUTOSTART : 0) |
				   (ctx->onConfigure.configCallback ? LM_CF_CONFIGURABLE : 0) |
				   (ctx->onNotify.notificationCallback ? LM_CF_NOTIFY : 0)};
	size_t nl = strnlen(ctx->appFriendlyName, LM_NAME_MAX - 1);
	uint32_t id = 0;
	DWORD st = request(c, LM_CONNECT, hdr, sizeof hdr, ctx->appFriendlyName, (uint32_t)nl, &id,
			   sizeof id);
	if (st != LM_OK) {
		lgLcdDisconnect(ci);
		return st;
	}
	ctx->connection = ci;
	return LM_OK;
}

DWORD lgLcdConnect(lgLcdConnectContext *ctx)
{
	if (!ctx)
		return LM_ERROR_INVALID_PARAMETER;
	lgLcdConnectContextEx ex = {ctx->appFriendlyName, ctx->isPersistent, ctx->isAutostartable,
				    ctx->onConfigure, -1, LGLCD_APPLET_CAP_BW, 0, {NULL, NULL}};
	DWORD st = lgLcdConnectEx(&ex);
	ctx->connection = ex.connection;
	return st;
}

DWORD lgLcdDisconnect(int connection)
{
	struct conn *c = conn_get(connection);
	if (!c)
		return LM_ERROR_INVALID_PARAMETER;
	if (!c->dead)
		request(c, LM_DISCONNECT, NULL, 0, NULL, 0, NULL, 0);
	shutdown(c->fd, SHUT_RDWR);
	if (!pthread_equal(pthread_self(), c->th))
		pthread_join(c->th, NULL);
	else
		pthread_detach(c->th);
	close(c->fd);
	pthread_mutex_destroy(&c->req);
	pthread_mutex_destroy(&c->m);
	pthread_cond_destroy(&c->cv);
	c->used = 0;
	return LM_OK;
}

DWORD lgLcdSetDeviceFamiliesToUse(int connection, DWORD families, DWORD reserved)
{
	struct conn *c = conn_get(connection);
	if (!c)
		return LM_ERROR_INVALID_PARAMETER;
	uint32_t f = families;
	return request(c, LM_FAMILIES, &f, 4, NULL, 0, NULL, 0);
}

DWORD lgLcdEnumerateEx(int connection, int index, lgLcdDeviceDescEx *d)
{
	struct conn *c = conn_get(connection);
	if (!c || !d || index < 0)
		return LM_ERROR_INVALID_PARAMETER;
	uint32_t i = (uint32_t)index;
	struct lm_devdesc dd;
	memset(&dd, 0, sizeof dd);
	DWORD st = request(c, LM_ENUMERATE, &i, 4, NULL, 0, &dd, sizeof dd);
	if (st == LM_OK) {
		memset(d, 0, sizeof *d);
		d->deviceFamilyId = dd.family;
		snprintf(d->deviceDisplayName, sizeof d->deviceDisplayName, "%.*s",
			 (int)sizeof dd.name, dd.name);
		d->Width = dd.width;
		d->Height = dd.height;
		d->Bpp = dd.bpp;
		d->NumSoftButtons = dd.softbuttons;
	}
	return st;
}

DWORD lgLcdEnumerate(int connection, int index, lgLcdDeviceDesc *d)
{
	lgLcdDeviceDescEx ex;
	if (!d)
		return LM_ERROR_INVALID_PARAMETER;
	DWORD st = lgLcdEnumerateEx(connection, index, &ex);
	if (st == LM_OK) {
		d->Width = ex.Width;
		d->Height = ex.Height;
		d->Bpp = ex.Bpp;
		d->NumSoftButtons = ex.NumSoftButtons;
	}
	return st;
}

static DWORD open_common(int connection, uint32_t by, uint32_t value,
			 lgLcdSoftbuttonsChangedContext sb, int *device)
{
	struct conn *c = conn_get(connection);
	if (!c || !device)
		return LM_ERROR_INVALID_PARAMETER;
	int di = -1;
	for (int i = 0; i < MAX_DEV; i++)
		if (!c->dev[i].used) {
			di = i;
			break;
		}
	if (di < 0)
		return LM_ERROR_NO_MORE_ITEMS;
	uint32_t req[2] = {by, value}, srv = 0;
	/* register the callback first: a button event may follow the reply at once */
	c->dev[di].sb = sb;
	c->dev[di].buttons = 0;
	DWORD st = request(c, LM_OPEN, req, sizeof req, NULL, 0, &srv, sizeof srv);
	if (st != LM_OK)
		return st;
	c->dev[di].srv = srv;
	c->dev[di].used = 1;
	*device = connection << 8 | di;
	return LM_OK;
}

DWORD lgLcdOpen(lgLcdOpenContext *ctx)
{
	if (!ctx)
		return LM_ERROR_INVALID_PARAMETER;
	return open_common(ctx->connection, 0, (uint32_t)ctx->index, ctx->onSoftbuttonsChanged,
			   &ctx->device);
}

DWORD lgLcdOpenByType(lgLcdOpenByTypeContext *ctx)
{
	if (!ctx)
		return LM_ERROR_INVALID_PARAMETER;
	return open_common(ctx->connection, 1, (uint32_t)ctx->deviceType,
			   ctx->onSoftbuttonsChanged, &ctx->device);
}

DWORD lgLcdClose(int device)
{
	struct dev *d;
	struct conn *c = dev_get(device, &d);
	if (!c)
		return LM_ERROR_INVALID_PARAMETER;
	uint32_t srv = d->srv;
	d->used = 0;
	return request(c, LM_CLOSE, &srv, 4, NULL, 0, NULL, 0);
}

DWORD lgLcdReadSoftButtons(int device, DWORD *buttons)
{
	struct dev *d;
	struct conn *c = dev_get(device, &d);
	if (!c || !buttons)
		return LM_ERROR_INVALID_PARAMETER;
	uint32_t srv = d->srv, b = 0;
	DWORD st = request(c, LM_READBUTTONS, &srv, 4, NULL, 0, &b, sizeof b);
	if (st == LM_OK)
		*buttons = b;
	return st;
}

DWORD lgLcdUpdateBitmap(int device, const lgLcdBitmapHeader *bmp, DWORD priority)
{
	struct dev *d;
	struct conn *c = dev_get(device, &d);
	if (!c || !bmp)
		return LM_ERROR_INVALID_PARAMETER;
	uint32_t n;
	const void *px;
	if (bmp->Format == LGLCD_BMP_FORMAT_QVGAx32) {
		n = LGLCD_QVGA_BMP_WIDTH * LGLCD_QVGA_BMP_HEIGHT * LGLCD_QVGA_BMP_BPP;
		px = ((const lgLcdBitmapQVGAx32 *)bmp)->pixels;
	} else if (bmp->Format == LGLCD_BMP_FORMAT_160x43x1) {
		n = LGLCD_BW_BMP_WIDTH * LGLCD_BW_BMP_HEIGHT;
		px = ((const lgLcdBitmap160x43x1 *)bmp)->pixels;
	} else {
		return LM_ERROR_INVALID_PARAMETER;
	}
	uint32_t h[3] = {d->srv, priority, bmp->Format};
	return request(c, LM_UPDATE, h, sizeof h, px, n, NULL, 0);
}

DWORD lgLcdSetAsLCDForegroundApp(int device, int yes)
{
	struct dev *d;
	struct conn *c = dev_get(device, &d);
	if (!c)
		return LM_ERROR_INVALID_PARAMETER;
	uint32_t r[2] = {d->srv, yes ? 1u : 0u};
	return request(c, LM_FOREGROUND, r, sizeof r, NULL, 0, NULL, 0);
}

DWORD tdxLcdSetIcon(int connection, int width, int height, const BYTE *rgba)
{
	struct conn *c = conn_get(connection);
	if (!c || !rgba || width < 1 || height < 1 || width > 64 || height > 64)
		return LM_ERROR_INVALID_PARAMETER;
	uint32_t h[2] = {(uint32_t)width, (uint32_t)height};
	return request(c, LM_ICON, h, sizeof h, rgba, (uint32_t)(width * height * 4), NULL, 0);
}
