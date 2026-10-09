#ifndef YTM_NET_H
#define YTM_NET_H

#include "app.h"

int net_init(char *err, int err_n);
void net_shutdown(void);

/* Fills out[] with up to max tracks. Returns the count, or -1 on transport error. */
int ytm_search(const char *query, Track *out, int max, char *err, int err_n);

/* Public Music shelf, such as new releases or a chart playlist. */
int ytm_browse(const char *browse_id, Track *out, int max, char *err, int err_n);

/* Writes a direct googlevideo audio URL. duration may be 0 if unknown. */
int ytm_audio_url(const char *video_id, char *url, int url_n, int *duration,
                  char *err, int err_n);
/* User-Agent and Referer that match the client which produced the last URL. */
const char *ytm_stream_ua(void);
const char *ytm_stream_referer(void);

/* YouTube TV device-code OAuth. Poll returns 0 when approved, 1 while waiting, -1 on failure. */
int ytm_auth_begin(char *user_code, int code_n, char *verify_url, int url_n, int *interval,
                   char *err, int err_n);
int ytm_auth_poll(char *err, int err_n);
int ytm_auth_refresh(char *err, int err_n);
void ytm_auth_signout(void);
int ytm_signed_in(void);
const char *ytm_refresh_token(void);
void ytm_set_refresh_token(const char *token);

/* Song radio (RDAMVM + the video id). 0-length means YouTube returned no mix. */
int ytm_radio(const char *video_id, Track *out, int max, char *err, int err_n);

/* Liked songs for the signed-in account. */
int ytm_liked(Track *out, int max, char *err, int err_n);

/* Home feed. Songs and playlists are filled separately. 0 if either side has rows.
 * Signed in, this is the account home and charts are not used. Signed out, charts fill
 * an empty side. Headings name the two columns. Place is a country code, or "Global". */
int ytm_home(Track *songs, int song_max, int *nsongs, Track *mixes, int mix_max, int *nmixes,
             char *err, int err_n);
const char *ytm_home_song_heading(void);
const char *ytm_home_mix_heading(void);
const char *ytm_home_place(void);

/* Rewrite url to the host that serves the audio. Headers only; the file is not read. */
int ytm_stream_follow(char *url, int url_n);

/* Download one cover into the cache. side >= 200 uses the Now Playing photo.
 * Smaller sides share the row cache and are not fetched again when home returns. */
int ytm_cover_fetch(const Track *t, int side);
const unsigned char *ytm_cover_pixels(const Track *t, int side, int *w, int *h);

/* Resolve the next track's audio URL and keep it until play. */
int ytm_prefetch_audio(const char *video_id);
const char *ytm_prefetch_url(const char *video_id, int *duration);
void ytm_prefetch_drop(const char *video_id);

#endif
