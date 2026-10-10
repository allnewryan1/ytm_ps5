#ifndef YTM_DAEMON_CLIENT_H
#define YTM_DAEMON_CLIENT_H

/* The background payload (ytmusicd). See ytmd.h for its lifecycle. */

/* Deploy it through the ELF loader and keep the loader connection as its channel. 0 when it
 * answered. Blocks for up to about six seconds while the loader starts it. */
int ytmd_start(void);
/* Call every frame. Pings every two seconds and redeploys a daemon that died (a few times). */
void ytmd_tick(void);
/* Tell it to quit and disconnect. A force-closed app needs no call: the daemon sees the
 * connection close and exits on its own. */
void ytmd_stop(void);

int ytmd_running(void);
int ytmd_pid(void);
/* One line for the Account page: running, or why not. */
const char *ytmd_status(void);

#endif
