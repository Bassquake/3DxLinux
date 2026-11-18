#define _GNU_SOURCE
#include "control.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int make_addr(const char *path, struct sockaddr_un *a)
{
	memset(a, 0, sizeof *a);
	a->sun_family = AF_UNIX;
	if (strlen(path) >= sizeof a->sun_path)
		return -ENAMETOOLONG;
	snprintf(a->sun_path, sizeof a->sun_path, "%s", path);
	return 0;
}

int tdx_ctl_listen(const char *path)
{
	struct sockaddr_un a;
	int rc = make_addr(path, &a);
	if (rc)
		return rc;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	unlink(path);
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	chmod(path, 0666); /* the focus helper runs in the user's session */
	return fd;
}

static int keep_conn;

void tdx_ctl_keep_connection(void)
{
	keep_conn = 1;
}

int tdx_ctl_serve(int lfd, tdx_ctl_handler h)
{
	int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return -1;
	keep_conn = 0;
	struct timeval tv = {.tv_sec = 0, .tv_usec = 300000}; /* never stall the daemon */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	char cmd[2048]; /* app-buttons carries up to a dozen key names */
	size_t n = 0;
	while (n < sizeof cmd - 1) {
		ssize_t r = read(fd, cmd + n, sizeof cmd - 1 - n);
		if (r <= 0)
			break;
		n += (size_t)r;
		if (memchr(cmd, '\n', n))
			break;
	}
	cmd[n] = 0;
	cmd[strcspn(cmd, "\r\n")] = 0;
	char reply[TDX_CTL_MAX];
	reply[0] = 0;
	if (n)
		h(cmd, reply, sizeof reply);
	size_t len = strlen(reply);
	if (len && write(fd, reply, len) < 0) {
		/* client went away */
		keep_conn = 0;
	}
	if (keep_conn) {
		keep_conn = 0;
		return fd;
	}
	close(fd);
	return -1;
}

int tdx_ctl_request(const char *path, const char *cmd, char *reply, size_t len)
{
	struct sockaddr_un a;
	int rc = make_addr(path, &a);
	if (rc)
		return rc;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	char line[512];
	snprintf(line, sizeof line, "%s\n", cmd);
	if (write(fd, line, strlen(line)) < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	size_t n = 0;
	for (;;) {
		ssize_t r = read(fd, reply + n, len - 1 - n);
		if (r <= 0 || n + (size_t)r >= len - 1) {
			if (r > 0)
				n += (size_t)r;
			break;
		}
		n += (size_t)r;
	}
	reply[n] = 0;
	close(fd);
	return 0;
}
