#ifndef YTM_APP_H
#define YTM_APP_H

#define YTM_TRACK_CAP 40
#define YTM_ID_LEN 12
#define YTM_TITLE_LEN 180

typedef struct Track {
  char id[YTM_ID_LEN];
  char title[YTM_TITLE_LEN];
  char artist[YTM_TITLE_LEN];
  char album[YTM_TITLE_LEN];
  int seconds;
} Track;

#endif
