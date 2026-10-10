#ifndef YTM_DAEMON_CLIENT_H
#define YTM_DAEMON_CLIENT_H

/* The background payload (ytmusicd). See ytmd.h for its lifecycle. */

/* Deploy it through the ELF loader if it is not running, then connect. 0 when connected.
 * Blocks for up to about six seconds while a new daemon starts. */
int ytmd_start(void);
/* Call every frame. Pings every two seconds and restarts a daemon that died (a few times). */
void ytmd_tick(void);
/* Tell it to quit and disconnect. A force-closed app needs no call: the daemon sees the
 * connection close and exits on its own. */
void ytmd_stop(void);

int ytmd_running(void);
int ytmd_pid(void);
/* One line for the Account page: running, or why not. */
const char *ytmd_status(void);

#endif
