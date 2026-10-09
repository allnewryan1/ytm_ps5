#ifndef YTM_APP_H
#define YTM_APP_H

#define YTM_TRACK_CAP 40
#define YTM_ID_LEN 12
#define YTM_TITLE_LEN 180
#define YTM_BROWSE_LEN 72
#define YTM_THUMB_LEN 200

typedef struct Track {
  char id[YTM_ID_LEN];
  char browse[YTM_BROWSE_LEN];
  char album_id[YTM_BROWSE_LEN];
  char artist_id[YTM_BROWSE_LEN];
  char title[YTM_TITLE_LEN];
  char artist[YTM_TITLE_LEN];
  char album[YTM_TITLE_LEN];
  char thumb[YTM_THUMB_LEN];
  int seconds;
  int video; /* 1 when YouTube marks the row as a video, not an audio track */
} Track;

#endif
