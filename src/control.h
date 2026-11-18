/*
 * tdxd control socket: one text command per connection, one text reply.
 *   status                 active profile, focused app, devices
 *   profiles               list profiles and what they match
 *   focus APP [PID]        the focused application changed (sent by tdx-focus)
 *   calibrate              re-zero every device (feature report 0x07)
 *   reload                 re-read the configuration and profiles (like SIGHUP)
 *   buttons                each key's number and V3DK id
 *   app-buttons PID [TAB V3DK TAB NAME]...
 *                          keys the application handles itself (PID 0: the
 *                          process on the other end of this connection)
 *   lcd-release            hand the SpacePilot Pro LCD to tdx-lcdmgr while this
 *                          connection stays open (tdxd takes it back on close)
 */
#ifndef TDX_CONTROL_H
#define TDX_CONTROL_H

#include <stddef.h>

#define TDX_CTL_MAX 4096

typedef void (*tdx_ctl_handler)(char *cmd, char *reply, size_t len);

/* Server side. Returns the listening fd or -errno. */
int tdx_ctl_listen(const char *path);
/* Accept one connection, read one line, call @h, send the reply, close.
 * If the handler called tdx_ctl_keep_connection(), the connection is left
 * open and its fd returned (else -1). */
int tdx_ctl_serve(int lfd, tdx_ctl_handler h);
void tdx_ctl_keep_connection(void);
/* While @h runs: the process id of the client, as the kernel gives it
 * (SO_PEERCRED: a real one even from a Flatpak sandbox), or 0. */
int tdx_ctl_peer_pid(void);
/* Client side: send @cmd, store the reply. Returns 0 or -errno. */
int tdx_ctl_request(const char *path, const char *cmd, char *reply, size_t len);

#endif
