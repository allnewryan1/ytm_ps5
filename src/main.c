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

#define RAIL 300
#define BAR_Y 908
#define HINT_Y 1016

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
    toast(err);
    return;
  }
  if (dur > 0) g_queue[idx].seconds = dur;
  if (player_start(url, g_queue[idx].seconds) != 0) {
    set_status(player_error()[0] ? player_error() : "Playback failed");
    toast(g_status);
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
    if (g_nresults > 0) play_list(g_results, g_nresults, 0);
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
  const int vis = 6;
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
  int tw;
  cover_rgb(label, &r, &g, &b);
  if (ring) fill_v(d, x - 6, y - 6, size + 12, size + 12, 212, 166, 86);
  fill_v(d, x, y, size, size, r, g, b);
  letter[0] = (label && label[0]) ? (char)toupper((unsigned char)label[0]) : 'M';
  letter[1] = 0;
  tw = text_px(letter, 1);
  draw_text(d, x + (size - tw) / 2, y + size / 2 - 18, 1, 255, 248, 236, letter);
}

static void present(Draw *d) {
  static int told;
  draw_end(d);
  if (SDL_UpdateWindowSurface(g_win) == 0) return;
  if (told) return;
  told = 1;
  toast(SDL_GetError());
}

static void draw_hints(Draw *d) {
  const char *s;
  fill_v(d, 0, HINT_Y, 1920, 1080 - HINT_Y, 10, 11, 14);
  fill_v(d, 0, HINT_Y, 1920, 2, 212, 166, 86);
  if (g_player_ui) {
    s = "X Pause    O Back    D-pad Seek / Volume    L1 R1 Skip    Square Like    Create Repeat";
  } else if (g_body == BODY_SEARCH) {
    s = "X Type    O Back    D-pad Move    Triangle Player    Square Like    Options Search";
  } else if (g_body == BODY_HOME) {
    s = "X Play    O Menu    D-pad Move    Options Search    Square Like    Triangle Player    L1 R1 Skip";
  } else {
    s = "X Play    O Home    D-pad Move    Square Like    Triangle Player    Options Search    L1 R1 Skip";
  }
  draw_text(d, 28, HINT_Y + 16, 1, 214, 216, 224, s);
}

static void paint(void) {
  Draw d;
  char line[180];
  SDL_Surface *surf = SDL_GetWindowSurface(g_win);
  const Track *now = (g_qindex >= 0 && g_qindex < g_nqueue) ? &g_queue[g_qindex] : NULL;
  if (!surf) return;
  draw_begin(&d, surf);
  fill_v(&d, 0, 0, 1920, 1080, 12, 13, 16);

  if (g_player_ui && now) {
    char a[16], b[16];
    int dur = (int)player_duration();
    int pos = (int)player_position();
    int w = 0;
    if (dur < 1) dur = now->seconds;
    fill_v(&d, 0, 0, 1920, 1080, 8, 9, 12);
    draw_cover(&d, 780, 96, 360, now->title, 0);
    fit(line, (int)sizeof line, now->title, 2, 1500);
    draw_text(&d, (1920 - text_px(line, 2)) / 2, 500, 2, 244, 242, 236, line);
    fit(line, (int)sizeof line, now->artist, 1, 1200);
    draw_text(&d, (1920 - text_px(line, 1)) / 2, 575, 1, 168, 170, 180, line);
    draw_text(&d, (1920 - text_px(library_has(now->id) ? "Saved on this console" : "Square saves this song", 1)) / 2,
              630, 1, library_has(now->id) ? 212 : 150, library_has(now->id) ? 166 : 152,
              library_has(now->id) ? 86 : 140,
              library_has(now->id) ? "Saved on this console" : "Square saves this song");
    fill_v(&d, 360, 700, 1200, 8, 40, 42, 52);
    if (dur > 0) {
      w = (int)(1200.0 * (pos / (double)dur));
      if (w < 0) w = 0;
      if (w > 1200) w = 1200;
      fill_v(&d, 360, 700, w, 8, 212, 166, 86);
    }
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, 360, 720, 1, 168, 170, 180, a);
    draw_text(&d, 1560 - text_px(b, 1), 720, 1, 168, 170, 180, b);
    snprintf(line, sizeof line, "%s     vol %d     %s", player_paused() ? "Paused" : "Playing",
             player_volume(),
             g_repeat == REP_ONE ? "Repeat one" : g_repeat == REP_ALL ? "Repeat all" : "Repeat off");
    draw_text(&d, (1920 - text_px(line, 1)) / 2, 790, 1, 196, 198, 206, line);
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, (1920 - text_px(line, 1)) / 2, 850, 1, 232, 120, 96, line);
    } else if (player_error()[0]) {
      fit(line, (int)sizeof line, player_error(), 1, 1400);
      draw_text(&d, (1920 - text_px(line, 1)) / 2, 850, 1, 232, 120, 96, line);
    }
    draw_hints(&d);
    present(&d);
    return;
  }

  fill_v(&d, 0, 0, RAIL, BAR_Y, 18, 19, 24);
  fill_v(&d, RAIL - 2, 0, 2, BAR_Y, 36, 38, 48);
  draw_disc(&d, 40, 52, 12, 212, 166, 86);
  draw_text(&d, 64, 22, 1, 244, 242, 236, "YouTube");
  draw_text(&d, 64, 54, 1, 168, 170, 180, "Music");

  for (int i = 0; i < 3; i++) {
    int y = 150 + i * 78;
    int on = (g_nav == i);
    if (on) {
      fill_v(&d, 0, y - 8, 6, 52, 212, 166, 86);
      if (g_zone == ZONE_NAV) fill_v(&d, 16, y - 8, RAIL - 32, 52, 32, 34, 44);
    }
    draw_text(&d, 36, y, 1, on ? 244 : 150, on ? 242 : 152, on ? 236 : 164, nav_name[i]);
  }
  {
    int on = (g_nav == NAV_QUIT);
    if (on && g_zone == ZONE_NAV) fill_v(&d, 16, BAR_Y - 78, RAIL - 32, 52, 32, 34, 44);
    if (on) fill_v(&d, 0, BAR_Y - 78, 6, 52, 212, 166, 86);
    draw_text(&d, 36, BAR_Y - 70, 1, on ? 244 : 120, on ? 242 : 122, on ? 236 : 132, "Quit");
  }

  if (g_body == BODY_HOME) {
    int pill = (g_zone == ZONE_BODY && g_home < 0);
    const char *hint = g_status[0] ? g_status : "Search songs, albums, artists";
    fill_v(&d, RAIL + 28, 24, 1560, 64, pill ? 244 : 28, pill ? 242 : 30, pill ? 236 : 38);
    fit(line, (int)sizeof line, hint, 1, 1500);
    draw_text(&d, RAIL + 48, 38, 1, pill ? 20 : (g_status[0] ? 232 : 150), pill ? 18 : (g_status[0] ? 120 : 152),
              pill ? 16 : (g_status[0] ? 96 : 164), line);
    draw_text(&d, RAIL + 28, 112, 1, 244, 242, 236, "Shelves");
    for (int i = 0; i < 8; i++) {
      int col = i % 4;
      int row = i / 4;
      int x = RAIL + 28 + col * 390;
      int y = row == 0 ? 160 : 480;
      int sel = (g_zone == ZONE_BODY && g_home == i);
      int cr, cg, cb;
      cover_rgb(moods[i][0], &cr, &cg, &cb);
      if (sel) fill_v(&d, x - 4, y - 4, 368, 308, 212, 166, 86);
      fill_v(&d, x, y, 360, 300, 24, 26, 34);
      fill_v(&d, x, y, 360, 8, cr, cg, cb);
      draw_cover(&d, x + 96, y + 36, 168, moods[i][0], 0);
      draw_text(&d, x + 24, y + 220, 1, 244, 242, 236, moods[i][0]);
      draw_text(&d, x + 24, y + 252, 1, 150, 152, 164, "X plays this shelf");
    }
  } else if (g_body == BODY_SEARCH) {
    fill_v(&d, RAIL + 28, 24, 1560, 64, 28, 30, 38);
    fit(line, (int)sizeof line, g_query[0] ? g_query : "Search songs, albums, artists", 1, 1500);
    draw_text(&d, RAIL + 48, 38, 1, g_query[0] ? 244 : 140, g_query[0] ? 242 : 142, g_query[0] ? 236 : 154, line);
    for (int r = 0; r < 5; r++) {
      int cols = key_cols(r);
      int indent = r == 2 ? 48 : r == 3 ? 96 : 0;
      for (int c = 0; c < cols; c++) {
        int x = RAIL + 28 + indent + c * (r == 4 ? 280 : 118);
        int y = 120 + r * 100;
        int sel = (g_zone == ZONE_BODY && g_keyr == r && g_keyc == c);
        char lab[8];
        int kw = r == 4 ? 250 : 104;
        fill_v(&d, x, y, kw, 84, sel ? 244 : 32, sel ? 242 : 34, sel ? 236 : 44);
        if (r < 4) {
          lab[0] = krow[r][c];
          lab[1] = 0;
          draw_text(&d, x + (kw - text_px(lab, 1)) / 2, y + 24, 1, sel ? 16 : 244, sel ? 16 : 242,
                    sel ? 18 : 236, lab);
        } else {
          draw_text(&d, x + 28, y + 24, 1, sel ? 16 : 244, sel ? 16 : 242, sel ? 18 : 236, specials[c]);
        }
      }
    }
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, RAIL + 28, 640, 1, 232, 120, 96, line);
    }
  } else {
    Track *list = g_body == BODY_LIBRARY ? g_likes : g_results;
    int n = g_body == BODY_LIBRARY ? g_nlikes : g_nresults;
    const char *heading = g_body == BODY_LIBRARY ? "Library" : (g_list_title[0] ? g_list_title : "Songs");
    const int vis = 6;
    fit(line, (int)sizeof line, heading, 2, 1200);
    draw_text(&d, RAIL + 28, 28, 2, 244, 242, 236, line);
    if (n == 0) {
      draw_text(&d, RAIL + 28, 160, 1, 168, 170, 180,
                g_body == BODY_LIBRARY ? "Songs you save show up here." : "No songs for that search.");
    }
    for (int i = 0; i < vis && g_scroll + i < n; i++) {
      int idx = g_scroll + i;
      int y = 130 + i * 120;
      int sel = (g_zone == ZONE_BODY && g_sel == idx);
      char time[16];
      if (sel) fill_v(&d, RAIL + 16, y - 8, 1588, 108, 32, 34, 44);
      if (sel) fill_v(&d, RAIL + 16, y - 8, 6, 108, 212, 166, 86);
      draw_cover(&d, RAIL + 36, y, 84, list[idx].title, 0);
      fit(line, (int)sizeof line, list[idx].title, 1, 980);
      draw_text(&d, RAIL + 140, y + 8, 1, 244, 242, 236, line);
      fit(line, (int)sizeof line, list[idx].artist, 1, 760);
      draw_text(&d, RAIL + 140, y + 46, 1, 150, 152, 164, line);
      if (library_has(list[idx].id)) draw_text(&d, RAIL + 1180, y + 24, 1, 212, 166, 86, "Saved");
      if (list[idx].seconds > 0) {
        fmt_time(time, (int)sizeof time, list[idx].seconds);
        draw_text(&d, RAIL + 1420, y + 24, 1, 150, 152, 164, time);
      }
    }
  }

  fill_v(&d, 0, BAR_Y, 1920, HINT_Y - BAR_Y, 22, 23, 30);
  fill_v(&d, 0, BAR_Y, 1920, 2, 48, 50, 60);
  if (now) {
    char a[16], b[16];
    int dur = (int)player_duration();
    int pos = (int)player_position();
    if (dur < 1) dur = now->seconds;
    draw_cover(&d, 20, BAR_Y + 14, 72, now->title, 0);
    fit(line, (int)sizeof line, now->title, 1, 460);
    draw_text(&d, 108, BAR_Y + 16, 1, 244, 242, 236, line);
    fit(line, (int)sizeof line, now->artist, 1, 460);
    draw_text(&d, 108, BAR_Y + 52, 1, 150, 152, 164, line);
    draw_text(&d, 620, BAR_Y + 32, 1, 196, 198, 206, player_paused() ? "Paused" : "Playing");
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, 820, BAR_Y + 16, 1, 168, 170, 180, a);
    fill_v(&d, 940, BAR_Y + 36, 620, 6, 48, 50, 60);
    if (dur > 0) {
      int w = (int)(620.0 * (pos / (double)dur));
      if (w < 0) w = 0;
      if (w > 620) w = 620;
      fill_v(&d, 940, BAR_Y + 36, w, 6, 212, 166, 86);
    }
    draw_text(&d, 1580, BAR_Y + 16, 1, 168, 170, 180, b);
    snprintf(line, sizeof line, "vol %d", player_volume());
    draw_text(&d, 1720, BAR_Y + 32, 1, 168, 170, 180, line);
  } else {
    draw_text(&d, 28, BAR_Y + 32, 1, 150, 152, 164, "Nothing playing");
  }
  draw_hints(&d);
  present(&d);
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
  if (player_open(err, (int)sizeof err) != 0) {
    set_status(err);
    toast(err);
  }
  if (net_init(err, (int)sizeof err) != 0) {
    set_status(err);
    toast(err);
  } else if (!g_status[0]) {
    set_status("Pick a shelf. X plays it.");
  }
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
