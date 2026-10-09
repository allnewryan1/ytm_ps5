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

enum { NAV_HOME, NAV_SEARCH, NAV_LIBRARY, NAV_ACCOUNT, NAV_QUIT, NAV_COUNT };
enum { ZONE_NAV, ZONE_BODY };
enum { BODY_HOME, BODY_LIST, BODY_SEARCH, BODY_LIBRARY, BODY_ACCOUNT, BODY_EXPLORE };
enum { REP_OFF, REP_ALL, REP_ONE };

static const char *nav_name[] = {"Home", "Explore", "Library", "Account", "Quit"};
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
static const char *explore_shelf[][3] = {
    {"New releases", "Albums and singles just out", "FEmusic_new_releases"},
    {"Charts", "Top songs", "VLPL4fGSI1pDJn4yCNzulPkUbxgr4pl0gmI-"},
    {"Trending", "What people are playing", "FEmusic_explore"},
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
static int g_explore = 0;
static int g_sel = 0;
static int g_scroll = 0;
static int g_keyr;
static int g_keyc;
static int g_player_ui;
static int g_repeat = REP_OFF;
static int g_acct_sel;
static int g_auth_wait;
static int g_auth_interval = 5;
static Uint32 g_auth_next;
static char g_user_code[24];
static char g_verify_url[80];

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

static void auth_save(void) {
  const char *t = ytm_refresh_token();
  FILE *f;
  if (!t || !t[0]) return;
  mkdir("/data", 0755);
  mkdir("/data/ytmusic", 0755);
  f = fopen("/data/ytmusic/auth.txt", "w");
  if (!f) {
    set_status("Could not save sign-in on this console");
    return;
  }
  fprintf(f, "%s\n", t);
  fclose(f);
}

static void auth_load(void) {
  FILE *f = fopen("/data/ytmusic/auth.txt", "r");
  char line[1100];
  char err[192];
  if (!f) return;
  if (!fgets(line, sizeof line, f)) {
    fclose(f);
    return;
  }
  fclose(f);
  line[strcspn(line, "\r\n")] = 0;
  if (!line[0]) return;
  ytm_set_refresh_token(line);
  if (ytm_auth_refresh(err, (int)sizeof err) != 0) set_status(err);
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

static void open_shelf(int idx) {
  char err[192];
  int n;
  if (idx < 0 || idx > 2) return;
  snprintf(g_list_title, sizeof g_list_title, "%s", explore_shelf[idx][0]);
  set_status("Loading...");
  paint();
  n = ytm_browse(explore_shelf[idx][2], g_results, YTM_TRACK_CAP, err, (int)sizeof err);
  if (n < 0) {
    g_nresults = 0;
    set_status(err);
    toast(err);
    return;
  }
  g_nresults = n;
  g_sel = 0;
  g_scroll = 0;
  g_body = BODY_LIST;
  g_zone = ZONE_BODY;
  if (n == 0) set_status(err[0] ? err : "Nothing in that shelf");
  else {
    char msg[64];
    snprintf(msg, sizeof msg, "%d songs", n);
    set_status(msg);
  }
}

static void open_library(void) {
  char err[192];
  int n;
  g_nav = NAV_LIBRARY;
  g_player_ui = 0;
  g_auth_wait = 0;
  if (!ytm_signed_in()) {
    g_nlikes = 0;
    g_sel = 0;
    g_scroll = 0;
    g_body = BODY_LIBRARY;
    g_zone = ZONE_BODY;
    set_status("Sign in from Account to load your library");
    return;
  }
  g_nlikes = 0;
  g_sel = 0;
  g_scroll = 0;
  set_status("Loading your library...");
  g_body = BODY_LIBRARY;
  g_zone = ZONE_BODY;
  paint();
  n = ytm_liked(g_likes, YTM_TRACK_CAP, err, (int)sizeof err);
  if (n < 0) {
    g_nlikes = 0;
    set_status(err[0] ? err : "Could not load your library");
    toast(g_status);
    return;
  }
  g_nlikes = n;
  if (n == 0) set_status(err[0] ? err : "No liked songs");
  else {
    char msg[64];
    snprintf(msg, sizeof msg, "%d songs", n);
    set_status(msg);
  }
}

static int key_cols(int row) { return row < 4 ? (int)strlen(krow[row]) : 4; }

static void start_code(void) {
  char err[192];
  int interval = 5;
  g_user_code[0] = 0;
  g_verify_url[0] = 0;
  if (ytm_auth_begin(g_user_code, (int)sizeof g_user_code, g_verify_url, (int)sizeof g_verify_url,
                     &interval, err, (int)sizeof err) != 0) {
    g_auth_wait = 0;
    set_status(err);
    toast(err);
    return;
  }
  g_auth_interval = interval;
  g_auth_next = SDL_GetTicks() + (Uint32)interval * 1000u;
  g_auth_wait = 1;
  set_status("Waiting for approval");
}

static void open_account(void) {
  char err[192];
  g_body = BODY_ACCOUNT;
  g_zone = ZONE_BODY;
  g_nav = NAV_ACCOUNT;
  g_acct_sel = 0;
  g_player_ui = 0;
  if (!ytm_signed_in() && ytm_refresh_token()[0] && ytm_auth_refresh(err, (int)sizeof err) != 0)
    set_status(err);
  if (ytm_signed_in()) {
    g_auth_wait = 0;
    set_status("Signed in");
    return;
  }
  start_code();
}

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
    else if (g_nav == NAV_SEARCH) {
      g_body = BODY_EXPLORE;
      g_explore = 0;
      set_status("");
    } else if (g_nav == NAV_LIBRARY) {
      open_library();
      return;
    } else {
      open_account();
      return;
    }
    g_auth_wait = 0;
    g_zone = ZONE_BODY;
    return;
  }
  if (g_body == BODY_HOME) {
    do_search(moods[g_home][1]);
    if (g_nresults > 0) play_list(g_results, g_nresults, 0);
  } else if (g_body == BODY_EXPLORE) {
    open_shelf(g_explore);
  } else if (g_body == BODY_LIST) {
    play_list(g_results, g_nresults, g_sel);
  } else if (g_body == BODY_LIBRARY) {
    if (!ytm_signed_in()) open_account();
    else if (g_nlikes > 0) play_list(g_likes, g_nlikes, g_sel);
    else open_library();
  } else if (g_body == BODY_ACCOUNT) {
    if (!ytm_signed_in()) {
      start_code();
      return;
    }
    if (g_acct_sel == 0) open_library();
    else {
      ytm_auth_signout();
      remove("/data/ytmusic/auth.txt");
      g_auth_wait = 0;
      set_status("Signed out");
      toast("Signed out");
      start_code();
    }
  } else if (g_body == BODY_SEARCH) {
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
  g_auth_wait = 0;
  if (g_body != BODY_HOME) {
    g_body = BODY_HOME;
    g_zone = ZONE_BODY;
    return;
  }
  g_zone = ZONE_NAV;
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
    int x = g_home % 4;
    int y = g_home / 4;
    x += dx;
    y += dy;
    if (x < 0) {
      g_zone = ZONE_NAV;
      return;
    }
    if (x > 3) x = 3;
    if (y < 0) y = 0;
    if (y > 1) y = 1;
    g_home = y * 4 + x;
    return;
  }
  if (g_body == BODY_EXPLORE) {
    if (dy) {
      g_explore += dy;
      if (g_explore < 0) g_explore = 0;
      if (g_explore > 2) g_explore = 2;
    }
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
  if (g_body == BODY_ACCOUNT) {
    if (dx < 0) {
      g_zone = ZONE_NAV;
      return;
    }
    if (ytm_signed_in() && dy) {
      g_acct_sel += dy;
      if (g_acct_sel < 0) g_acct_sel = 0;
      if (g_acct_sel > 1) g_acct_sel = 1;
    }
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

static void draw_repeat_icon(Draw *d, int cx, int cy, int mode, int hr, int hg, int hb) {
  int on = mode != REP_OFF;
  int rad = 22;
  draw_disc(d, cx, cy, rad, on ? 212 : 110, on ? 166 : 112, on ? 86 : 120);
  draw_disc(d, cx, cy, 14, hr, hg, hb);
  if (mode == REP_ONE) {
    int tw = text_px("1", 1);
    draw_text(d, cx - tw / 2, cy - 16, 1, 244, 242, 236, "1");
  } else if (mode == REP_OFF) {
    for (int i = -16; i <= 16; i++) fill_v(d, cx + i, cy - i, 2, 2, 168, 170, 178);
  }
}

static void draw_volume(Draw *d, int x, int y, int w) {
  int vol = player_volume();
  int fw;
  int knob;
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  fill_round(d, x, y, w, 8, 4, 40, 42, 52);
  fw = (w * vol) / 100;
  if (fw > 0) fill_round(d, x, y, fw, 8, 4, 212, 166, 86);
  knob = x + fw;
  if (knob < x + 10) knob = x + 10;
  if (knob > x + w - 10) knob = x + w - 10;
  draw_disc(d, knob, y + 4, 11, 244, 242, 236);
}

static void draw_cover(Draw *d, int x, int y, int size, const char *label, int ring) {
  int r, g, b;
  char letter[2];
  int tw;
  int rad = size > 200 ? 28 : size > 100 ? 20 : 16;
  cover_rgb(label, &r, &g, &b);
  if (ring) fill_round(d, x - 6, y - 6, size + 12, size + 12, rad + 6, 212, 166, 86);
  fill_round(d, x, y, size, size, rad, r, g, b);
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
    s = "X Pause   O Back   D-pad Seek / Volume   L1 R1 Skip   Touchpad Repeat";
  } else if (g_body == BODY_ACCOUNT) {
    s = ytm_signed_in()
            ? "X Open   O Back   D-pad Move   Options Search   Touchpad Repeat"
            : "X New code   O Back   Options Search";
  } else if (g_body == BODY_SEARCH) {
    s = "X Type   O Back   D-pad Move   Triangle Player   Touchpad Repeat";
  } else if (g_body == BODY_HOME) {
    s = "X Play   O Menu   D-pad Move   Options Search   Touchpad Repeat   Triangle Player";
  } else if (g_body == BODY_EXPLORE) {
    s = "X Open   O Home   D-pad Move   Options Search   Touchpad Repeat";
  } else if (g_body == BODY_LIBRARY) {
    s = ytm_signed_in()
            ? "X Play   O Home   D-pad Move   Options Search   Touchpad Repeat   Triangle Player"
            : "X Sign in   O Home   Options Search   Touchpad Repeat";
  } else {
    s = "X Play   O Home   D-pad Move   Triangle Player   Options Search   Touchpad Repeat";
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
    fill_round(&d, 28, 24, 1864, HINT_Y - 48, 36, 212, 166, 86);
    fill_round(&d, 36, 32, 1848, HINT_Y - 64, 32, 8, 9, 12);
    fill_round(&d, 56, 48, 380, 44, 22, 212, 166, 86);
    draw_text(&d, 76, 54, 1, 24, 18, 8, "Controlling the player");
    draw_cover(&d, 780, 96, 360, now->title, 0);
    fit(line, (int)sizeof line, now->title, 2, 1500);
    draw_text(&d, (1920 - text_px(line, 2)) / 2, 500, 2, 244, 242, 236, line);
    fit(line, (int)sizeof line, now->artist, 1, 1200);
    draw_text(&d, (1920 - text_px(line, 1)) / 2, 575, 1, 168, 170, 180, line);
    if (now->album[0]) {
      fit(line, (int)sizeof line, now->album, 1, 1200);
      draw_text(&d, (1920 - text_px(line, 1)) / 2, 618, 1, 150, 152, 164, line);
    }
    fill_round(&d, 360, 680, 1200, 8, 4, 40, 42, 52);
    if (dur > 0) {
      w = (int)(1200.0 * (pos / (double)dur));
      if (w < 0) w = 0;
      if (w > 1200) w = 1200;
      if (w > 0) fill_round(&d, 360, 680, w, 8, 4, 212, 166, 86);
    }
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, 360, 700, 1, 168, 170, 180, a);
    draw_text(&d, 1560 - text_px(b, 1), 700, 1, 168, 170, 180, b);
    draw_text(&d, 470, 760, 1, 150, 152, 164, "Volume");
    draw_volume(&d, 640, 772, 640);
    draw_repeat_icon(&d, 1340, 776, g_repeat, 8, 9, 12);
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, (1920 - text_px(line, 1)) / 2, 830, 1, 232, 120, 96, line);
    } else if (player_error()[0]) {
      fit(line, (int)sizeof line, player_error(), 1, 1400);
      draw_text(&d, (1920 - text_px(line, 1)) / 2, 830, 1, 232, 120, 96, line);
    } else if (player_paused()) {
      draw_text(&d, (1920 - text_px("Paused", 1)) / 2, 830, 1, 196, 198, 206, "Paused");
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
  {
    const char *mode = "Choosing a section";
    if (g_zone != ZONE_NAV) {
      if (g_body == BODY_HOME) mode = "Shelves";
      else if (g_body == BODY_EXPLORE) mode = "Explore";
      else if (g_body == BODY_SEARCH) mode = "Searching";
      else if (g_body == BODY_LIBRARY) mode = "Your library";
      else if (g_body == BODY_ACCOUNT) mode = ytm_signed_in() ? "Signed in" : "Signing in";
      else mode = "Song list";
    }
    draw_text(&d, 28, 104, 1, 212, 166, 86, mode);
  }

  for (int i = 0; i < NAV_QUIT; i++) {
    int y = 156 + i * 68;
    int on = (g_nav == i);
    if (on) {
      fill_round(&d, 16, y - 8, RAIL - 32, 48, 24, g_zone == ZONE_NAV ? 54 : 32,
                 g_zone == ZONE_NAV ? 44 : 34, g_zone == ZONE_NAV ? 28 : 44);
    }
    draw_text(&d, 36, y, 1, on ? 244 : 150, on ? 242 : 152, on ? 236 : 164, nav_name[i]);
  }
  {
    int on = (g_nav == NAV_QUIT);
    if (on) fill_round(&d, 16, BAR_Y - 78, RAIL - 32, 52, 24, g_zone == ZONE_NAV ? 54 : 32,
                       g_zone == ZONE_NAV ? 44 : 34, g_zone == ZONE_NAV ? 28 : 44);
    draw_text(&d, 36, BAR_Y - 70, 1, on ? 244 : 120, on ? 242 : 122, on ? 236 : 132, "Quit");
  }

  if (g_body == BODY_HOME) {
    draw_text(&d, RAIL + 28, 24, 1, 244, 242, 236, "Shelves");
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, RAIL + 28, 68, 1, 232, 160, 110, line);
    }
    for (int i = 0; i < 8; i++) {
      int col = i % 4;
      int row = i / 4;
      int x = RAIL + 28 + col * 390;
      int y = 140 + row * 250;
      int sel = (g_zone == ZONE_BODY && g_home == i);
      int cr, cg, cb;
      cover_rgb(moods[i][0], &cr, &cg, &cb);
      if (sel) fill_round(&d, x - 4, y - 4, 368, 208, 28, 212, 166, 86);
      fill_round(&d, x, y, 360, 200, 24, 24, 26, 34);
      fill_round(&d, x + 24, y + 36, 72, 8, 4, cr, cg, cb);
      draw_text(&d, x + 24, y + 64, 2, 244, 242, 236, moods[i][0]);
      draw_text(&d, x + 24, y + 140, 1, 150, 152, 164, "X plays");
    }
  } else if (g_body == BODY_EXPLORE) {
    draw_text(&d, RAIL + 28, 24, 2, 244, 242, 236, "Explore");
    draw_text(&d, RAIL + 28, 96, 1, 150, 152, 164, "New music, charts, and what is playing now");
    for (int i = 0; i < 3; i++) {
      int y = 160 + i * 220;
      int sel = (g_zone == ZONE_BODY && g_explore == i);
      int cr, cg, cb;
      cover_rgb(explore_shelf[i][0], &cr, &cg, &cb);
      if (sel) fill_round(&d, RAIL + 20, y - 4, 1548, 200, 28, 212, 166, 86);
      fill_round(&d, RAIL + 28, y, 1532, 188, 24, 24, 26, 34);
      fill_v(&d, RAIL + 28, y + 28, 8, 132, cr, cg, cb);
      draw_text(&d, RAIL + 64, y + 48, 2, 244, 242, 236, explore_shelf[i][0]);
      draw_text(&d, RAIL + 64, y + 118, 1, 150, 152, 164, explore_shelf[i][1]);
    }
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, RAIL + 28, 830, 1, 232, 160, 110, line);
    }
  } else if (g_body == BODY_SEARCH) {
    fill_round(&d, RAIL + 28, 24, 1560, 64, 32, 28, 30, 38);
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
        fill_round(&d, x, y, kw, 84, 20, sel ? 244 : 32, sel ? 242 : 34, sel ? 236 : 44);
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
  } else if (g_body == BODY_ACCOUNT) {
    const char *shown = g_verify_url;
    draw_text(&d, RAIL + 28, 28, 2, 244, 242, 236, ytm_signed_in() ? "Account" : "Sign in");
    if (!ytm_signed_in()) {
      if (strncmp(shown, "https://", 8) == 0) shown += 8;
      else if (strncmp(shown, "http://", 7) == 0) shown += 7;
      draw_text(&d, RAIL + 28, 120, 1, 168, 170, 180, "On a phone or computer, open");
      draw_text(&d, RAIL + 28, 164, 1, 212, 166, 86, shown[0] ? shown : "google.com/device");
      draw_text(&d, RAIL + 28, 220, 1, 168, 170, 180, "and enter this code");
      fill_round(&d, RAIL + 28, 268, 560, 84, 24, 28, 30, 38);
      draw_text(&d, RAIL + 52, 292, 2, 244, 242, 236, g_user_code[0] ? g_user_code : "...");
      if (g_status[0]) {
        fit(line, (int)sizeof line, g_status, 1, 1400);
        draw_text(&d, RAIL + 28, 380, 1, 232, 160, 110, line);
      }
      draw_text(&d, RAIL + 28, 450, 1, 150, 152, 164, "X asks for a new code if this one expires");
    } else {
      const char *rows[] = {"Liked songs", "Sign out"};
      draw_text(&d, RAIL + 28, 120, 1, 168, 170, 180, "YouTube Music is linked to this console");
      for (int i = 0; i < 2; i++) {
        int y = 200 + i * 100;
        int sel = (g_zone == ZONE_BODY && g_acct_sel == i);
        if (sel) fill_round(&d, RAIL + 16, y, 900, 80, 24, 42, 36, 28);
        draw_text(&d, RAIL + 40, y + 22, 1, 244, 242, 236, rows[i]);
      }
    }
  } else {
    Track *list = g_body == BODY_LIBRARY ? g_likes : g_results;
    int n = g_body == BODY_LIBRARY ? g_nlikes : g_nresults;
    const char *heading = g_body == BODY_LIBRARY ? "Library" : (g_list_title[0] ? g_list_title : "Songs");
    const int vis = 6;
    fit(line, (int)sizeof line, heading, 2, 1200);
    draw_text(&d, RAIL + 28, 28, 2, 244, 242, 236, line);
    if (n == 0) {
      const char *empty = "No songs for that search.";
      if (g_status[0]) empty = g_status;
      else if (g_body == BODY_LIBRARY)
        empty = ytm_signed_in() ? "No liked songs on this account."
                                : "Sign in from Account to load your library.";
      fit(line, (int)sizeof line, empty, 1, 1400);
      draw_text(&d, RAIL + 28, 160, 1, 168, 170, 180, line);
    }
    for (int i = 0; i < vis && g_scroll + i < n; i++) {
      int idx = g_scroll + i;
      int y = 130 + i * 120;
      int sel = (g_zone == ZONE_BODY && g_sel == idx);
      char time[16];
      if (sel) fill_round(&d, RAIL + 16, y - 8, 1588, 108, 24, 32, 34, 44);
      draw_cover(&d, RAIL + 36, y, 84, list[idx].title, 0);
      fit(line, (int)sizeof line, list[idx].title, 1, 1100);
      draw_text(&d, RAIL + 140, y + 8, 1, 244, 242, 236, line);
      fit(line, (int)sizeof line, list[idx].artist, 1, 900);
      draw_text(&d, RAIL + 140, y + 46, 1, 150, 152, 164, line);
      if (list[idx].seconds > 0) {
        fmt_time(time, (int)sizeof time, list[idx].seconds);
        draw_text(&d, RAIL + 1420, y + 24, 1, 150, 152, 164, time);
      }
    }
  }

  fill_round(&d, 16, BAR_Y + 8, 1888, HINT_Y - BAR_Y - 16, 28, 28, 30, 38);
  if (now) {
    char a[16], b[16];
    const char *sub = now->album[0] ? now->album : now->artist;
    int dur = (int)player_duration();
    int pos = (int)player_position();
    int bar_x = 700;
    int bar_w = 720;
    if (dur < 1) dur = now->seconds;
    draw_cover(&d, 36, BAR_Y + 18, 72, now->title, 0);
    fit(line, (int)sizeof line, now->title, 1, 400);
    draw_text(&d, 124, BAR_Y + 20, 1, 244, 242, 236, line);
    fit(line, (int)sizeof line, sub, 1, 400);
    draw_text(&d, 124, BAR_Y + 54, 1, 150, 152, 164, line);
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, bar_x - 16 - text_px(a, 1), BAR_Y + 38, 1, 168, 170, 180, a);
    fill_round(&d, bar_x, BAR_Y + 50, bar_w, 8, 4, 48, 50, 60);
    if (dur > 0) {
      int bw = (int)((double)bar_w * (pos / (double)dur));
      if (bw < 0) bw = 0;
      if (bw > bar_w) bw = bar_w;
      if (bw > 0) fill_round(&d, bar_x, BAR_Y + 50, bw, 8, 4, 212, 166, 86);
    }
    draw_text(&d, bar_x + bar_w + 16, BAR_Y + 38, 1, 168, 170, 180, b);
    draw_repeat_icon(&d, 1816, BAR_Y + 54, g_repeat, 28, 30, 38);
  } else {
    draw_text(&d, 36, BAR_Y + 36, 1, 150, 152, 164, "Nothing playing");
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
      } else if (k == SDLK_f) g_player_ui = !g_player_ui;
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
  if (edge(SDL_CONTROLLER_BUTTON_Y)) g_player_ui = !g_player_ui;
  /* This pad reports the touchpad as start and Options as back. */
  if (edge(SDL_CONTROLLER_BUTTON_BACK)) {
    g_body = BODY_SEARCH;
    g_zone = ZONE_BODY;
    g_player_ui = 0;
    g_nav = NAV_SEARCH;
    g_auth_wait = 0;
  }
  if (edge(SDL_CONTROLLER_BUTTON_START)) {
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
  {
    int net_ok = net_init(err, (int)sizeof err) == 0;
    if (!net_ok) {
      set_status(err);
      toast(err);
    } else if (!g_status[0]) {
      set_status("Pick a shelf. X plays it.");
    }
    if (net_ok) auth_load();
  }

  while (g_run) {
    poll_input();
    if (g_auth_wait && g_body == BODY_ACCOUNT && !g_player_ui && !ytm_signed_in() &&
        SDL_GetTicks() >= g_auth_next) {
      char aerr[192];
      int rc;
      g_auth_next = SDL_GetTicks() + (Uint32)g_auth_interval * 1000u;
      rc = ytm_auth_poll(aerr, (int)sizeof aerr);
      if (rc == 0) {
        g_auth_wait = 0;
        auth_save();
        set_status("Signed in");
        toast("Signed in to YouTube Music");
      } else if (rc < 0) {
        g_auth_wait = 0;
        set_status(aerr);
        toast(aerr);
      }
    }
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
