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

/* YouTube TV device-code OAuth. Poll returns 0 when approved, 1 while waiting, -1 on failure. */
int ytm_auth_begin(char *user_code, int code_n, char *verify_url, int url_n, int *interval,
                   char *err, int err_n);
int ytm_auth_poll(char *err, int err_n);
int ytm_auth_refresh(char *err, int err_n);
void ytm_auth_signout(void);
int ytm_signed_in(void);
const char *ytm_refresh_token(void);
void ytm_set_refresh_token(const char *token);

/* Liked songs for the signed-in account. */
int ytm_liked(Track *out, int max, char *err, int err_n);

#endif
