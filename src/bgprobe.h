#ifndef YTM_BGPROBE_H
#define YTM_BGPROBE_H

/* Background audio test (Account page). Runs on its own thread. -1 if one is running. */
int bgprobe_start(void);
int bgprobe_running(void);

#endif
