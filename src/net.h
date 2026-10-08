#ifndef YTM_NET_H
#define YTM_NET_H

#include "app.h"

int net_init(char *err, int err_n);
void net_shutdown(void);

/* Fills out[] with up to max tracks. Returns the count, or -1 on transport error. */
int ytm_search(const char *query, Track *out, int max, char *err, int err_n);

/* Writes a direct googlevideo audio URL. duration may be 0 if unknown. */
int ytm_audio_url(const char *video_id, char *url, int url_n, int *duration,
                  char *err, int err_n);

#endif
