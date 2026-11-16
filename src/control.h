/*
 * tdxd control socket: one text command per connection, one text reply.
 *   status                 active profile, focused app, devices
 *   profiles               list profiles and what they match
 *   focus APP [PID]        the focused application changed (sent by tdx-focus)
 *   calibrate              re-zero every device (feature report 0x07)
 *   reload                 re-read the configuration and profiles (like SIGHUP)
 */
#ifndef TDX_CONTROL_H
#define TDX_CONTROL_H

#include <stddef.h>

#define TDX_CTL_MAX 4096

typedef void (*tdx_ctl_handler)(char *cmd, char *reply, size_t len);

/* Server side. Returns the listening fd or -errno. */
int tdx_ctl_listen(const char *path);
/* Accept one connection, read one line, call @h, send the reply, close. */
void tdx_ctl_serve(int lfd, tdx_ctl_handler h);
/* Client side: send @cmd, store the reply. Returns 0 or -errno. */
int tdx_ctl_request(const char *path, const char *cmd, char *reply, size_t len);

#endif
