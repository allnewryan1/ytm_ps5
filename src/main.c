#include "app.h"
#include "net.h"
#include "player.h"
#include "text.h"

#include <SDL.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RAIL 280
#define BOTTOM 972

enum { NAV_HOME, NAV_SEARCH, NAV_LIBRARY, NAV_QUIT, NAV_COUNT };
enum { ZONE_NAV, ZONE_BODY };
enum { BODY_HOME, BODY_LIST, BODY_SEARCH, BODY_LIBRARY };
enum { REP_OFF, REP_ALL, REP_ONE };

static const char *nav_name[] = {"Home", "Explore", "Library", "Quit"};
static const char *krow[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *specials[] = {"space", "del", "clear", "search"};
static const char *moods[][2] = {
    {"Chill", "chill lo-fi beats"},
    {"Focus", "focus study music"},
    {"Energy", "workout electronic"},
    {"Hip-hop", "hip hop mix"},
    {"Jazz", "jazz instrumental"},
    {"Rock", "rock hits"},
    {"Pop", "pop hits"},
    {"Night", "late night drive"},
};

static SDL_Window *g_win;
static SDL_GameController *g_pad;
static Uint8 g_prev[32];
static int g_hold;
static Uint32 g_hold_next;

static int g_run = 1;
static int g_nav = NAV_HOME;
static int g_zone = ZONE_BODY;
static int g_body = BODY_HOME;
static int g_home = 0;
static int g_sel = 0;
static int g_scroll = 0;
static int g_keyr;
static int g_keyc;
static int g_player_ui;
static int g_repeat = REP_OFF;

static Track g_results[YTM_TRACK_CAP];
static int g_nresults;
static Track g_likes[YTM_TRACK_CAP];
static int g_nlikes;
static Track g_queue[YTM_TRACK_CAP];
static int g_nqueue;
static int g_qindex = -1;
static char g_url[4096];
static char g_query[81];
static char g_list_title[80];
static char g_status[220];

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelUsleep(unsigned int micros);

static void toast(const char *msg) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static void park(const char *msg) {
  printf("ytmusic: %s\n", msg ? msg : "stopped");
  toast(msg && msg[0] ? msg : "YouTube Music stopped");
  for (;;) sceKernelUsleep(1000000);
}

__attribute__((weak)) int ytm_heap_error(void) { return 0; }

static void park_err(const char *extra) {
  char msg[140];
  const char *err = SDL_GetError();
  int heap = ytm_heap_error();
  if (!err || !err[0]) err = "stopped";
  if (heap && extra && extra[0])
    snprintf(msg, sizeof msg, "%s %s (heap %d)", err, extra, heap);
  else if (heap)
    snprintf(msg, sizeof msg, "%s (heap %d)", err, heap);
  else if (extra && extra[0])
    snprintf(msg, sizeof msg, "%s %s", err, extra);
  else
    snprintf(msg, sizeof msg, "%s", err);
  park(msg);
}

static void set_status(const char *s) { snprintf(g_status, sizeof g_status, "%s", s ? s : ""); }

static void fmt_time(char *d, int n, int sec) {
  int m, s;
  if (sec < 0) sec = 0;
  m = sec / 60;
  s = sec % 60;
  if (m >= 60) snprintf(d, (size_t)n, "%d:%02d:%02d", m / 60, m % 60, s);
  else snprintf(d, (size_t)n, "%d:%02d", m, s);
}

static void fit(char *dst, int n, const char *src, int scale, int max_px) {
  int full;
  snprintf(dst, (size_t)n, "%s", src ? src : "");
  if (text_px(dst, scale) <= max_px) return;
  full = (int)strlen(dst);
  while (dst[0] && text_px(dst, scale) > max_px) dst[strlen(dst) - 1] = 0;
  if ((int)strlen(dst) < full && (int)strlen(dst) >= 2) {
    int L = (int)strlen(dst);
    dst[L - 1] = '.';
    dst[L - 2] = '.';
  }
}

static void library_load(void) {
  FILE *f = fopen("/data/ytmusic/liked.txt", "r");
  char line[512];
  g_nlikes = 0;
  if (!f) return;
  while (g_nlikes < YTM_TRACK_CAP && fgets(line, sizeof line, f)) {
    char *tab1;
    char *tab2;
    char *tab3;
    Track *t = &g_likes[g_nlikes];
    line[strcspn(line, "\r\n")] = 0;
    tab1 = strchr(line, '\t');
    if (!tab1) continue;
    *tab1 = 0;
    tab2 = strchr(tab1 + 1, '\t');
    if (!tab2) continue;
    *tab2 = 0;
    tab3 = strchr(tab2 + 1, '\t');
    if (tab3) *tab3 = 0;
    if (!line[0]) continue;
    memset(t, 0, sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", line);
    snprintf(t->title, sizeof t->title, "%s", tab1 + 1);
    snprintf(t->artist, sizeof t->artist, "%s", tab2 + 1);
    if (tab3) t->seconds = atoi(tab3 + 1);
    g_nlikes++;
  }
  fclose(f);
}

static void library_save(void) {
  FILE *f;
  mkdir("/data", 0755);
  mkdir("/data/ytmusic", 0755);
  f = fopen("/data/ytmusic/liked.txt", "w");
  if (!f) {
    set_status("Library needs a writable /data/ytmusic");
    return;
  }
  for (int i = 0; i < g_nlikes; i++) {
    char title[YTM_TITLE_LEN];
    char artist[YTM_TITLE_LEN];
    snprintf(title, sizeof title, "%s", g_likes[i].title);
    snprintf(artist, sizeof artist, "%s", g_likes[i].artist);
    for (int c = 0; title[c]; c++)
      if (title[c] == '\t' || title[c] == '\n') title[c] = ' ';
    for (int c = 0; artist[c]; c++)
      if (artist[c] == '\t' || artist[c] == '\n') artist[c] = ' ';
    fprintf(f, "%s\t%s\t%s\t%d\n", g_likes[i].id, title, artist, g_likes[i].seconds);
  }
  fclose(f);
}

static int library_has(const char *id) {
  for (int i = 0; i < g_nlikes; i++)
    if (strcmp(g_likes[i].id, id) == 0) return 1;
  return 0;
}

static void library_toggle(const Track *t) {
  if (!t || !t->id[0]) return;
  for (int i = 0; i < g_nlikes; i++) {
    if (strcmp(g_likes[i].id, t->id) == 0) {
      memmove(&g_likes[i], &g_likes[i + 1], (size_t)(g_nlikes - i - 1) * sizeof(Track));
      g_nlikes--;
      library_save();
      set_status("Removed from library");
      return;
    }
  }
  if (g_nlikes >= YTM_TRACK_CAP) {
    memmove(&g_likes[1], &g_likes[0], (size_t)(YTM_TRACK_CAP - 1) * sizeof(Track));
    g_likes[0] = *t;
  } else {
    memmove(&g_likes[1], &g_likes[0], (size_t)g_nlikes * sizeof(Track));
    g_likes[0] = *t;
    g_nlikes++;
  }
  library_save();
  set_status("Saved to this console");
}

static void paint(void);

static void do_search(const char *q) {
  char err[192];
  int n;
  snprintf(g_query, sizeof g_query, "%s", q);
  snprintf(g_list_title, sizeof g_list_title, "%s", q);
  set_status("Searching YouTube Music...");
  paint();
  n = ytm_search(q, g_results, YTM_TRACK_CAP, err, (int)sizeof err);
  if (n < 0) {
    g_nresults = 0;
    set_status(err);
    return;
  }
  g_nresults = n;
  g_sel = 0;
  g_scroll = 0;
  g_body = BODY_LIST;
  g_zone = ZONE_BODY;
  if (n == 0) set_status(err[0] ? err : "No songs");
  else {
    char msg[64];
    snprintf(msg, sizeof msg, "%d songs", n);
    set_status(msg);
  }
}

static void play_queue_index(int idx) {
  char err[192];
  char url[4096];
  int dur = 0;
  if (idx < 0 || idx >= g_nqueue) return;
  snprintf(g_status, sizeof g_status, "Opening %s", g_queue[idx].title);
  paint();
  if (ytm_audio_url(g_queue[idx].id, url, (int)sizeof url, &dur, err, (int)sizeof err) != 0) {
    set_status(err);
    return;
  }
  if (dur > 0) g_queue[idx].seconds = dur;
  if (player_start(url, g_queue[idx].seconds) != 0) {
    set_status(player_error()[0] ? player_error() : "Playback failed");
    return;
  }
  snprintf(g_url, sizeof g_url, "%s", url);
  g_qindex = idx;
  g_player_ui = 1;
  set_status("");
}

static void play_list(Track *list, int n, int idx) {
  if (n <= 0 || idx < 0 || idx >= n) return;
  if (n > YTM_TRACK_CAP) n = YTM_TRACK_CAP;
  memcpy(g_queue, list, (size_t)n * sizeof(Track));
  g_nqueue = n;
  play_queue_index(idx);
}

static void maybe_advance(void) {
  if (!player_ended()) return;
  player_ack_ended();
  if (player_error()[0]) {
    set_status(player_error());
    return;
  }
  if (g_repeat == REP_ONE && g_url[0] && g_qindex >= 0) {
    player_start(g_url, g_queue[g_qindex].seconds);
    return;
  }
  if (g_qindex + 1 < g_nqueue) play_queue_index(g_qindex + 1);
  else if (g_repeat == REP_ALL && g_nqueue > 0) play_queue_index(0);
}

static Track *focused(void) {
  if (g_player_ui && g_qindex >= 0 && g_qindex < g_nqueue) return &g_queue[g_qindex];
  if (g_body == BODY_LIST && g_sel >= 0 && g_sel < g_nresults) return &g_results[g_sel];
  if (g_body == BODY_LIBRARY && g_sel >= 0 && g_sel < g_nlikes) return &g_likes[g_sel];
  if (g_qindex >= 0 && g_qindex < g_nqueue) return &g_queue[g_qindex];
  return NULL;
}

static int key_cols(int row) { return row < 4 ? (int)strlen(krow[row]) : 4; }

static void on_activate(void) {
  if (g_player_ui) {
    player_toggle();
    return;
  }
  if (g_zone == ZONE_NAV) {
    if (g_nav == NAV_QUIT) {
      g_run = 0;
      return;
    }
    if (g_nav == NAV_HOME) g_body = BODY_HOME;
    else if (g_nav == NAV_SEARCH) g_body = BODY_SEARCH;
    else {
      library_load();
      g_body = BODY_LIBRARY;
      g_sel = 0;
      g_scroll = 0;
    }
    g_zone = ZONE_BODY;
    return;
  }
  if (g_body == BODY_HOME) {
    if (g_home < 0) {
      g_body = BODY_SEARCH;
      g_nav = NAV_SEARCH;
      g_zone = ZONE_BODY;
      return;
    }
    do_search(moods[g_home][1]);
  }
  else if (g_body == BODY_LIST) play_list(g_results, g_nresults, g_sel);
  else if (g_body == BODY_LIBRARY) play_list(g_likes, g_nlikes, g_sel);
  else if (g_body == BODY_SEARCH) {
    if (g_keyr < 4) {
      if ((int)strlen(g_query) + 1 < (int)sizeof g_query) {
        int n = (int)strlen(g_query);
        g_query[n] = krow[g_keyr][g_keyc];
        g_query[n + 1] = 0;
      }
    } else if (g_keyc == 0) {
      if ((int)strlen(g_query) + 1 < (int)sizeof g_query) {
        int n = (int)strlen(g_query);
        g_query[n] = ' ';
        g_query[n + 1] = 0;
      }
    } else if (g_keyc == 1) {
      int n = (int)strlen(g_query);
      if (n > 0) g_query[n - 1] = 0;
    } else if (g_keyc == 2) {
      g_query[0] = 0;
    } else if (g_query[0]) {
      do_search(g_query);
    }
  }
}

static void on_back(void) {
  if (g_player_ui) {
    g_player_ui = 0;
    return;
  }
  if (g_body != BODY_HOME) {
    g_body = BODY_HOME;
    g_zone = ZONE_BODY;
    return;
  }
  g_zone = ZONE_NAV;
}

static void on_like(void) {
  Track *t = focused();
  if (t) library_toggle(t);
}

static void keep_sel_visible(int n) {
  const int vis = 8;
  if (n < 1) {
    g_sel = 0;
    g_scroll = 0;
    return;
  }
  if (g_sel < 0) g_sel = 0;
  if (g_sel >= n) g_sel = n - 1;
  if (g_sel < g_scroll) g_scroll = g_sel;
  if (g_sel >= g_scroll + vis) g_scroll = g_sel - vis + 1;
}

static void on_move(int dx, int dy) {
  if (g_player_ui) {
    if (dx) player_seek_by(dx * 10.0);
    if (dy) player_volume_add(-dy * 5);
    return;
  }
  if (g_zone == ZONE_NAV) {
    if (dx > 0) g_zone = ZONE_BODY;
    if (dy) {
      g_nav += dy;
      if (g_nav < 0) g_nav = 0;
      if (g_nav >= NAV_COUNT) g_nav = NAV_COUNT - 1;
    }
    return;
  }
  if (dx < 0 && g_body != BODY_SEARCH) {
    g_zone = ZONE_NAV;
    return;
  }
  if (g_body == BODY_HOME) {
    if (g_home < 0) {
      if (dy > 0) g_home = 0;
      if (dx < 0) g_zone = ZONE_NAV;
      return;
    }
    int x = g_home % 4;
    int y = g_home / 4;
    x += dx;
    y += dy;
    if (x < 0) {
      g_zone = ZONE_NAV;
      return;
    }
    if (x > 3) x = 3;
    if (y < 0) {
      g_home = -1;
      return;
    }
    if (y > 1) y = 1;
    g_home = y * 4 + x;
    return;
  }
  if (g_body == BODY_SEARCH) {
    g_keyr += dy;
    g_keyc += dx;
    if (g_keyr < 0) g_keyr = 0;
    if (g_keyr > 4) g_keyr = 4;
    if (g_keyc < 0) {
      g_zone = ZONE_NAV;
      g_keyc = 0;
      return;
    }
    if (g_keyc >= key_cols(g_keyr)) g_keyc = key_cols(g_keyr) - 1;
    return;
  }
  g_sel += dy;
  keep_sel_visible(g_body == BODY_LIBRARY ? g_nlikes : g_nresults);
}

static void cover_rgb(const char *s, int *r, int *g, int *b) {
  static const int pal[][3] = {
      {186, 28, 44},  {28, 92, 168}, {112, 42, 158}, {18, 122, 108},
      {176, 88, 24},  {58, 58, 158}, {150, 36, 88},  {36, 108, 58},
  };
  unsigned h = 7;
  if (!s) s = "";
  for (int i = 0; s[i]; i++) h = h * 33u + (unsigned char)s[i];
  *r = pal[h % 8][0];
  *g = pal[h % 8][1];
  *b = pal[h % 8][2];
}

static void draw_disc(Draw *d, int cx, int cy, int rad, int r, int g, int b) {
  for (int y = -rad; y <= rad; y++) {
    int lo = 0, hi = rad, span;
    while (lo < hi) {
      int mid = (lo + hi + 1) / 2;
      if (mid * mid + y * y <= rad * rad) lo = mid;
      else hi = mid - 1;
    }
    span = lo;
    fill_v(d, cx - span, cy + y, span * 2 + 1, 1, r, g, b);
  }
}

static void draw_cover(Draw *d, int x, int y, int size, const char *label, int ring) {
  int r, g, b;
  char letter[2];
  cover_rgb(label, &r, &g, &b);
  if (ring) fill_v(d, x - 4, y - 4, size + 8, size + 8, 255, 255, 255);
  fill_v(d, x, y, size, size, r, g, b);
  letter[0] = (label && label[0]) ? (char)toupper((unsigned char)label[0]) : 'M';
  letter[1] = 0;
  draw_text(d, x + size / 2 - 12, y + size / 2 - 16, size > 120 ? 5 : 3, 255, 255, 255, letter);
}

static void present(void) {
  static int told;
  if (SDL_UpdateWindowSurface(g_win) == 0) return;
  if (told) return;
  told = 1;
  toast(SDL_GetError());
}

static void paint(void) {
  Draw d;
  char line[180];
  SDL_Surface *surf = SDL_GetWindowSurface(g_win);
  const Track *now = (g_qindex >= 0 && g_qindex < g_nqueue) ? &g_queue[g_qindex] : NULL;
  if (!surf) return;
  draw_begin(&d, surf);

  fill_v(&d, 0, 0, 1920, 1080, 3, 3, 3);

  if (g_player_ui && now) {
    char a[16], b[16];
    int dur = (int)player_duration();
    int pos = (int)player_position();
    int w = 0;
    if (dur < 1) dur = now->seconds;
    fill_v(&d, 0, 0, 1920, 1080, 0, 0, 0);
    draw_cover(&d, 750, 80, 420, now->title, 0);
    fit(line, (int)sizeof line, now->title, 5, 1400);
    draw_text(&d, (1920 - text_px(line, 5)) / 2, 530, 5, 255, 255, 255, line);
    fit(line, (int)sizeof line, now->artist, 3, 1000);
    draw_text(&d, (1920 - text_px(line, 3)) / 2, 590, 3, 170, 170, 170, line);
    draw_text(&d, 860, 650, 2, library_has(now->id) ? 255 : 160, library_has(now->id) ? 0 : 160,
              library_has(now->id) ? 0 : 160, library_has(now->id) ? "Liked" : "Like");
    fill_v(&d, 360, 720, 1200, 4, 70, 70, 70);
    if (dur > 0) {
      w = (int)(1200.0 * (pos / (double)dur));
      if (w < 0) w = 0;
      if (w > 1200) w = 1200;
      fill_v(&d, 360, 720, w, 4, 255, 255, 255);
    }
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, 360, 736, 2, 168, 168, 168, a);
    draw_text(&d, 1480, 736, 2, 168, 168, 168, b);
    draw_disc(&d, 960, 840, 36, 255, 255, 255);
    draw_text(&d, 948, 824, 3, 0, 0, 0, player_paused() ? "II" : ">");
    draw_text(&d, 820, 824, 3, 220, 220, 220, "|<");
    draw_text(&d, 1060, 824, 3, 220, 220, 220, ">|");
    snprintf(line, sizeof line, "vol %d    %s", player_volume(),
             g_repeat == REP_ONE ? "repeat one" : g_repeat == REP_ALL ? "repeat all" : "repeat off");
    draw_text(&d, 760, 910, 2, 140, 140, 140, line);
    if (player_error()[0]) {
      fit(line, (int)sizeof line, player_error(), 2, 1200);
      draw_text(&d, 360, 960, 2, 255, 80, 80, line);
    }
    present();
    return;
  }

  fill_v(&d, 0, 0, RAIL, BOTTOM, 0, 0, 0);
  fill_v(&d, 28, 28, 36, 36, 255, 0, 0);
  draw_text(&d, 38, 36, 2, 255, 255, 255, ">");
  draw_text(&d, 76, 32, 2, 255, 255, 255, "YouTube");
  draw_text(&d, 76, 52, 2, 255, 255, 255, "Music");

  for (int i = 0; i < 3; i++) {
    int y = 140 + i * 72;
    int on = (g_nav == i);
    if (on && g_zone == ZONE_NAV) fill_v(&d, 16, y - 10, RAIL - 32, 52, 28, 28, 28);
    draw_text(&d, 36, y, 3, on ? 255 : 168, on ? 255 : 168, on ? 255 : 168, nav_name[i]);
  }
  {
    int on = (g_nav == NAV_QUIT);
    if (on && g_zone == ZONE_NAV) fill_v(&d, 16, 880, RAIL - 32, 44, 28, 28, 28);
    draw_text(&d, 36, 890, 2, on ? 255 : 110, on ? 255 : 110, on ? 255 : 110, "Quit");
  }

  if (g_body == BODY_HOME) {
    int pill = (g_zone == ZONE_BODY && g_home < 0);
    const char *hint = g_status[0] ? g_status : "Search songs, albums, artists";
    if (pill) fill_v(&d, RAIL + 28, 28, 1500, 56, 255, 255, 255);
    else fill_v(&d, RAIL + 28, 28, 1500, 56, 33, 33, 33);
    fit(line, (int)sizeof line, hint, 2, 1440);
    draw_text(&d, RAIL + 52, 44, 2, pill ? 20 : (g_status[0] ? 255 : 150), pill ? 20 : 150,
              pill ? 20 : 150, line);
    draw_text(&d, RAIL + 36, 112, 3, 255, 255, 255, "Mixed for you");
    draw_text(&d, RAIL + 36, 430, 3, 255, 255, 255, "Moods & genres");
    for (int i = 0; i < 8; i++) {
      int col = i % 4;
      int row = i / 4;
      int x = RAIL + 36 + col * 390;
      int y = row == 0 ? 164 : 482;
      int sel = (g_zone == ZONE_BODY && g_home == i);
      draw_cover(&d, x, y, 200, moods[i][0], sel);
      draw_text(&d, x, y + 212, 2, 255, 255, 255, moods[i][0]);
      draw_text(&d, x, y + 234, 2, 150, 150, 150, "Station");
    }
  } else if (g_body == BODY_SEARCH) {
    fill_v(&d, RAIL + 28, 28, 1500, 56, 33, 33, 33);
    fit(line, (int)sizeof line, g_query[0] ? g_query : "Search songs, albums, artists", 3, 1440);
    draw_text(&d, RAIL + 52, 42, 3, g_query[0] ? 255 : 140, g_query[0] ? 255 : 140, g_query[0] ? 255 : 140,
              line);
    fill_v(&d, RAIL + 36, 108, 140, 36, 255, 255, 255);
    draw_text(&d, RAIL + 56, 116, 2, 0, 0, 0, "Songs");
    draw_text(&d, RAIL + 200, 116, 2, 180, 180, 180, "Albums");
    draw_text(&d, RAIL + 340, 116, 2, 180, 180, 180, "Artists");
    draw_text(&d, RAIL + 490, 116, 2, 180, 180, 180, "Playlists");
    for (int r = 0; r < 5; r++) {
      int cols = key_cols(r);
      int indent = r == 2 ? 54 : r == 3 ? 108 : 0;
      for (int c = 0; c < cols; c++) {
        int x = RAIL + 36 + indent + c * (r == 4 ? 250 : 112);
        int y = 180 + r * 96;
        int sel = (g_zone == ZONE_BODY && g_keyr == r && g_keyc == c);
        char lab[8];
        fill_v(&d, x, y, r == 4 ? 230 : 100, 80, sel ? 255 : 40, sel ? 255 : 40, sel ? 255 : 40);
        if (r < 4) {
          lab[0] = krow[r][c];
          lab[1] = 0;
          draw_text(&d, x + 34, y + 24, 4, sel ? 0 : 255, sel ? 0 : 255, sel ? 0 : 255, lab);
        } else {
          draw_text(&d, x + 24, y + 28, 2, sel ? 0 : 255, sel ? 0 : 255, sel ? 0 : 255, specials[c]);
        }
      }
    }
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 2, 1400);
      draw_text(&d, RAIL + 36, 680, 2, 255, 120, 120, line);
    }
  } else {
    Track *list = g_body == BODY_LIBRARY ? g_likes : g_results;
    int n = g_body == BODY_LIBRARY ? g_nlikes : g_nresults;
    const char *heading = g_body == BODY_LIBRARY ? "Library" : (g_list_title[0] ? g_list_title : "Songs");
    const int vis = 8;
    fit(line, (int)sizeof line, heading, 5, 1100);
    draw_text(&d, RAIL + 36, 36, 5, 255, 255, 255, line);
    draw_disc(&d, RAIL + 1280, 56, 26, 255, 0, 0);
    draw_text(&d, RAIL + 1270, 44, 3, 255, 255, 255, ">");
    draw_text(&d, RAIL + 1320, 44, 2, 255, 255, 255, "Play");
    if (g_body == BODY_LIBRARY) {
      fill_v(&d, RAIL + 36, 100, 120, 32, 255, 255, 255);
      draw_text(&d, RAIL + 52, 108, 2, 0, 0, 0, "Songs");
    }
    if (n == 0) {
      draw_text(&d, RAIL + 36, 180, 3, 170, 170, 170,
                g_body == BODY_LIBRARY ? "Songs you like show up here." : "No songs for that search.");
    }
    for (int i = 0; i < vis && g_scroll + i < n; i++) {
      int idx = g_scroll + i;
      int y = 150 + i * 96;
      int sel = (g_zone == ZONE_BODY && g_sel == idx);
      char time[16];
      if (sel) fill_v(&d, RAIL + 20, y - 8, 1580, 88, 33, 33, 33);
      draw_cover(&d, RAIL + 36, y, 64, list[idx].title, 0);
      fit(line, (int)sizeof line, list[idx].title, 3, 980);
      draw_text(&d, RAIL + 120, y + 6, 3, 255, 255, 255, line);
      fit(line, (int)sizeof line, list[idx].artist, 2, 700);
      draw_text(&d, RAIL + 120, y + 38, 2, 170, 170, 170, line);
      if (library_has(list[idx].id)) draw_text(&d, RAIL + 1280, y + 20, 2, 255, 0, 0, "Liked");
      if (list[idx].seconds > 0) {
        fmt_time(time, (int)sizeof time, list[idx].seconds);
        draw_text(&d, RAIL + 1460, y + 20, 2, 170, 170, 170, time);
      }
    }
  }

  fill_v(&d, 0, BOTTOM, 1920, 1080 - BOTTOM, 33, 33, 33);
  if (now) {
    char a[16], b[16];
    int dur = (int)player_duration();
    int pos = (int)player_position();
    if (dur < 1) dur = now->seconds;
    draw_cover(&d, 16, BOTTOM + 22, 64, now->title, 0);
    fit(line, (int)sizeof line, now->title, 2, 420);
    draw_text(&d, 96, BOTTOM + 28, 2, 255, 255, 255, line);
    fit(line, (int)sizeof line, now->artist, 2, 420);
    draw_text(&d, 96, BOTTOM + 52, 2, 170, 170, 170, line);
    draw_text(&d, 620, BOTTOM + 36, 2, 200, 200, 200, "|<");
    draw_disc(&d, 760, BOTTOM + 48, 22, 255, 255, 255);
    draw_text(&d, 750, BOTTOM + 36, 2, 0, 0, 0, player_paused() ? "II" : ">");
    draw_text(&d, 860, BOTTOM + 36, 2, 200, 200, 200, ">|");
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, 980, BOTTOM + 20, 2, 160, 160, 160, a);
    fill_v(&d, 1080, BOTTOM + 28, 480, 4, 90, 90, 90);
    if (dur > 0) {
      int w = (int)(480.0 * (pos / (double)dur));
      if (w < 0) w = 0;
      if (w > 480) w = 480;
      fill_v(&d, 1080, BOTTOM + 28, w, 4, 255, 255, 255);
    }
    draw_text(&d, 1580, BOTTOM + 20, 2, 160, 160, 160, b);
    snprintf(line, sizeof line, "vol %d", player_volume());
    draw_text(&d, 1720, BOTTOM + 36, 2, 180, 180, 180, line);
  } else {
    draw_text(&d, 36, BOTTOM + 40, 2, 150, 150, 150, "Nothing playing");
  }
  present();
}

static int edge(SDL_GameControllerButton b) {
  int now = SDL_GameControllerGetButton(g_pad, b);
  int fire = now && !g_prev[b];
  g_prev[b] = (Uint8)now;
  return fire;
}

static void hold_dir(int dx, int dy) {
  int dir = dy < 0 ? 1 : dy > 0 ? 2 : dx < 0 ? 3 : dx > 0 ? 4 : 0;
  Uint32 now = SDL_GetTicks();
  if (!dir) {
    g_hold = 0;
    return;
  }
  if (dir != g_hold) {
    g_hold = dir;
    g_hold_next = now + 320;
    on_move(dx, dy);
    return;
  }
  if (now >= g_hold_next) {
    g_hold_next = now + 130;
    on_move(dx, dy);
  }
}

static void poll_input(void) {
  SDL_Event e;
  int dx = 0, dy = 0;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) g_run = 0;
    if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad)
      g_pad = SDL_GameControllerOpen(e.cdevice.which);
    if (e.type == SDL_TEXTINPUT && g_body == BODY_SEARCH && !g_player_ui) {
      const char *t = e.text.text;
      if (t && t[0] >= 32 && t[0] < 127 && (int)strlen(g_query) + 1 < (int)sizeof g_query) {
        int n = (int)strlen(g_query);
        g_query[n] = t[0];
        g_query[n + 1] = 0;
      }
    }
    if (e.type == SDL_KEYDOWN) {
      SDL_Keycode k = e.key.keysym.sym;
      if (k == SDLK_UP) on_move(0, -1);
      else if (k == SDLK_DOWN) on_move(0, 1);
      else if (k == SDLK_LEFT) on_move(-1, 0);
      else if (k == SDLK_RIGHT) on_move(1, 0);
      else if (k == SDLK_RETURN || k == SDLK_SPACE) on_activate();
      else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) {
        if (g_body == BODY_SEARCH && !g_player_ui && k == SDLK_BACKSPACE && g_query[0]) {
          g_query[strlen(g_query) - 1] = 0;
        } else on_back();
      } else if (k == SDLK_l) on_like();
      else if (k == SDLK_f) g_player_ui = !g_player_ui;
    }
  }
  if (!g_pad) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
      if (SDL_IsGameController(i)) {
        g_pad = SDL_GameControllerOpen(i);
        memset(g_prev, 0, sizeof g_prev);
        break;
      }
    }
  }
  if (!g_pad) return;

  if (edge(SDL_CONTROLLER_BUTTON_A)) on_activate();
  if (edge(SDL_CONTROLLER_BUTTON_B)) on_back();
  if (edge(SDL_CONTROLLER_BUTTON_X)) on_like();
  if (edge(SDL_CONTROLLER_BUTTON_Y)) g_player_ui = !g_player_ui;
  if (edge(SDL_CONTROLLER_BUTTON_START)) {
    g_body = BODY_SEARCH;
    g_zone = ZONE_BODY;
    g_player_ui = 0;
    g_nav = NAV_SEARCH;
  }
  if (edge(SDL_CONTROLLER_BUTTON_BACK)) {
    g_repeat = (g_repeat + 1) % 3;
    set_status(g_repeat == REP_ONE ? "Repeat one" : g_repeat == REP_ALL ? "Repeat all" : "Repeat off");
  }
  if (edge(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
    if (g_qindex > 0) play_queue_index(g_qindex - 1);
    else if (g_qindex == 0) player_seek_by(-1e9);
  }
  if (edge(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) && g_qindex + 1 < g_nqueue)
    play_queue_index(g_qindex + 1);

  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) dy = -1;
  else if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) dy = 1;
  else if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) dx = -1;
  else if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dx = 1;
  else {
    int ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (ax > 20000 || ax < -20000 || ay > 20000 || ay < -20000) {
      if (abs(ax) > abs(ay)) dx = ax > 0 ? 1 : -1;
      else dy = ay > 0 ? 1 : -1;
    }
  }
  /* Keep d-pad edges honest even when we drive movement from hold_dir. */
  (void)SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
  hold_dir(dx, dy);
}

int main(int argc, char **argv) {
  char err[192];
  (void)argc;
  (void)argv;
  printf("ytmusic: starting on PS5 userland\n");
  toast("YouTube Music");

  /* Audio or the pad must not take the picture down with them. */
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) park_err(0);
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    printf("ytmusic: audio: %s\n", SDL_GetError());
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
    printf("ytmusic: pad: %s\n", SDL_GetError());
  /* This SDL port has a window framebuffer and no render driver. */
  SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  g_win = SDL_CreateWindow("YouTube Music", 0, 0, 1920, 1080, 0);
  if (!g_win) park_err(0);
  if (!SDL_GetWindowSurface(g_win)) {
    char wh[32];
    int w = 0, h = 0;
    SDL_GetWindowSize(g_win, &w, &h);
    snprintf(wh, sizeof wh, "%dx%d", w, h);
    park_err(wh);
  }
  SDL_StartTextInput();
  if (player_open(err, (int)sizeof err) != 0) set_status(err);
  if (net_init(err, (int)sizeof err) != 0) set_status(err);
  else set_status("Pick a shelf, or Options to search.");
  library_load();

  while (g_run) {
    poll_input();
    maybe_advance();
    paint();
    SDL_Delay(16);
  }

  player_close();
  net_shutdown();
  if (g_pad) SDL_GameControllerClose(g_pad);
  SDL_DestroyWindow(g_win);
  SDL_Quit();
  return 0;
}
