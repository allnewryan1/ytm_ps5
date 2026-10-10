#include "app.h"
#include "bgprobe.h"
#include "daemon_client.h"
#include "net.h"
#include "player.h"
#include "text.h"

#include <SDL.h>

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RAIL 300
#define BAR_Y 908
#define HINT_Y 1016

/* Material 3 dark color roles (red tonal palette) and the shape scale.
 * https://m3.material.io/styles/color/roles  https://m3.material.io/styles/shape */
#define M3_PRIMARY 255, 180, 171
#define M3_ON_PRIMARY 105, 0, 5
#define M3_PRIMARY_CTN 147, 0, 10
#define M3_ON_PRIMARY_CTN 255, 218, 214
#define M3_SECONDARY_CTN 93, 63, 60
#define M3_ON_SECONDARY_CTN 255, 218, 214
#define M3_SURFACE 20, 18, 18
#define M3_SURF_LOWEST 15, 13, 13
#define M3_SURF_LOW 31, 26, 25
#define M3_SURF_CTN 36, 30, 29
#define M3_SURF_HIGH 46, 40, 39
#define M3_SURF_HIGHEST 57, 51, 50
#define M3_ON_SURFACE 240, 222, 220
#define M3_ON_SURFACE_VAR 215, 194, 191
#define M3_OUTLINE 160, 140, 137
#define M3_OUTLINE_VAR 83, 67, 65
#define M3_ERROR 242, 184, 181

enum { NAV_HOME, NAV_SEARCH, NAV_EXPLORE, NAV_LIBRARY, NAV_ACCOUNT, NAV_QUIT, NAV_COUNT };
enum { ZONE_NAV, ZONE_BODY };
enum { BODY_HOME, BODY_LIST, BODY_SEARCH, BODY_LIBRARY, BODY_ACCOUNT, BODY_EXPLORE };
enum { REP_OFF, REP_ALL, REP_ONE };

static const char *nav_name[] = {"Home", "Search", "Explore", "Library", "Account", "Quit"};
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
static int g_trig_l;
static int g_trig_r;
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
static Track g_scratch[YTM_TRACK_CAP];
static int g_qindex = -1;
/* 1 while the queue is an automix that may be topped up when it runs out. */
static int g_queue_mix;
static int g_qpick;
static int g_menu;
static int g_menu_sel;
static int g_menu_n;
static const char *g_menu_label[6];
static int g_menu_act[6];
static char g_query[81];
static char g_list_title[80];
static char g_status[220];

#define BACK_MAX 8
typedef struct {
  int body;
  int nav;
  int zone;
  int sel;
  int scroll;
  int home;
  int explore;
  int keyr;
  int keyc;
  int nresults;
  char title[80];
  Track results[YTM_TRACK_CAP];
} BackFrame;
static BackFrame g_back[BACK_MAX];
static int g_nback;

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

static void remember_here(void) {
  BackFrame *f;
  if (g_nback >= BACK_MAX) {
    memmove(&g_back[0], &g_back[1], (size_t)(BACK_MAX - 1) * sizeof(BackFrame));
    g_nback = BACK_MAX - 1;
  }
  f = &g_back[g_nback++];
  memset(f, 0, sizeof *f);
  f->body = g_body;
  f->nav = g_nav;
  f->zone = g_zone;
  f->sel = g_sel;
  f->scroll = g_scroll;
  f->home = g_home;
  f->explore = g_explore;
  f->keyr = g_keyr;
  f->keyc = g_keyc;
  f->nresults = g_nresults;
  snprintf(f->title, sizeof f->title, "%s", g_list_title);
  if (g_nresults > 0)
    memcpy(f->results, g_results, (size_t)g_nresults * sizeof(Track));
}

static void restore_here(void) {
  BackFrame *f;
  if (g_nback <= 0) {
    g_body = BODY_HOME;
    g_zone = ZONE_BODY;
    g_nav = NAV_HOME;
    return;
  }
  f = &g_back[--g_nback];
  g_body = f->body;
  g_nav = f->nav;
  g_zone = f->zone;
  g_sel = f->sel;
  g_scroll = f->scroll;
  g_home = f->home;
  g_explore = f->explore;
  g_keyr = f->keyr;
  g_keyc = f->keyc;
  g_nresults = f->nresults;
  snprintf(g_list_title, sizeof g_list_title, "%s", f->title);
  if (g_nresults > 0)
    memcpy(g_results, f->results, (size_t)g_nresults * sizeof(Track));
}

static void fmt_time(char *d, int n, int sec) {
  int m, s;
  if (sec < 0) sec = 0;
  m = sec / 60;
  s = sec % 60;
  if (m >= 60) snprintf(d, (size_t)n, "%d:%02d:%02d", m / 60, m % 60, s);
  else snprintf(d, (size_t)n, "%d:%02d", m, s);
}

/* Remove the last UTF-8 character of s. */
static void utf8_pop(char *s) {
  size_t L = strlen(s);
  if (L == 0) return;
  L--;
  while (L > 0 && ((unsigned char)s[L] & 0xC0) == 0x80) L--;
  s[L] = 0;
}

static void fit(char *dst, int n, const char *src, int scale, int max_px) {
  int full;
  if (!dst || n < 1) return;
  /* Callers pass the same buffer as src after writing it. Copying onto itself is undefined. */
  if (dst != src) snprintf(dst, (size_t)n, "%s", src ? src : "");
  if (text_px(dst, scale) <= max_px) return;
  full = (int)strlen(dst);
  /* Drop whole characters, so a cut never leaves half of a multi-byte one (drawn as '?'). */
  while (dst[0] && text_px(dst, scale) > max_px) utf8_pop(dst);
  if ((int)strlen(dst) < full && dst[0] && dst[1]) {
    utf8_pop(dst);
    if (dst[0]) utf8_pop(dst);
    snprintf(dst + strlen(dst), (size_t)n - strlen(dst), "..");
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
  /* The second line says which OAuth client minted the token; refresh must use the same one. */
  fprintf(f, "%s\n%s\n", t, ytm_token_custom() ? "client=custom" : "client=tv");
  fclose(f);
}

static void trim_line(char *s) {
  char *a = s;
  size_t n;
  s[strcspn(s, "\r\n")] = 0;
  while (*a == ' ' || *a == '\t') a++;
  if (a != s) memmove(s, a, strlen(a) + 1);
  n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

/* /data/ytmusic/oauth_client.txt: the client id and secret of a Google Cloud OAuth client of
 * type "TVs and Limited Input devices", one per line (or client_id=... / client_secret=...).
 * This is what ytmusicapi asks for; see README. */
static void oauth_client_load(void) {
  FILE *f = fopen("/data/ytmusic/oauth_client.txt", "r");
  char line[300];
  char id[200];
  char secret[120];
  int plain = 0;
  id[0] = 0;
  secret[0] = 0;
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char *v;
    trim_line(line);
    if (!line[0] || line[0] == '#') continue;
    v = strchr(line, '=');
    if (v && strncmp(line, "client_id", 9) == 0) {
      snprintf(id, sizeof id, "%s", v + 1);
      trim_line(id);
    } else if (v && strncmp(line, "client_secret", 13) == 0) {
      snprintf(secret, sizeof secret, "%s", v + 1);
      trim_line(secret);
    } else if (!v && plain == 0) {
      snprintf(id, sizeof id, "%s", line);
      plain = 1;
    } else if (!v && plain == 1) {
      snprintf(secret, sizeof secret, "%s", line);
      plain = 2;
    }
  }
  fclose(f);
  if (id[0] && secret[0]) ytm_set_oauth_client(id, secret);
}

static void auth_load(void) {
  FILE *f = fopen("/data/ytmusic/auth.txt", "r");
  char line[1100];
  char kind[64];
  char err[192];
  oauth_client_load();
  if (!f) return;
  if (!fgets(line, sizeof line, f)) {
    fclose(f);
    return;
  }
  kind[0] = 0;
  if (!fgets(kind, sizeof kind, f)) kind[0] = 0;
  fclose(f);
  line[strcspn(line, "\r\n")] = 0;
  if (!line[0]) return;
  ytm_set_refresh_token(line);
  ytm_set_token_custom(strncmp(kind, "client=custom", 13) == 0);
  if (ytm_auth_refresh(err, (int)sizeof err) != 0) set_status(err);
}

static void paint(void);
static void keep_sel_visible(int n);

/* typed is 1 for the search keyboard. Mood shortcuts search without typing into the bar. */
static void do_search(const char *q, int typed) {
  char err[192];
  int n;
  int from = g_body;
  if (from != BODY_LIST && from != BODY_SEARCH) remember_here();
  /* The keyboard passes g_query itself; copying a buffer onto itself is undefined. */
  if (typed && q != g_query) snprintf(g_query, sizeof g_query, "%s", q);
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

/* Play queue row idx. 0 when it started. On failure nothing keeps playing and the queue
 * points at the row that failed, so the queue and the speaker never disagree. */
static int play_queue_index(int idx) {
  char err[192];
  char url[8192];
  int dur = 0;
  if (idx < 0 || idx >= g_nqueue) return -1;
  snprintf(g_status, sizeof g_status, "Opening %s", g_queue[idx].title);
  paint();
  g_qindex = idx;
  g_qpick = idx;
  if (ytm_audio_url(g_queue[idx].id, url, (int)sizeof url, &dur, err, (int)sizeof err) != 0) {
    player_stop();
    set_status(err);
    toast(err);
    return -1;
  }
  if (dur > 0) g_queue[idx].seconds = dur;
  /* Headers only. The decoder streams the file on its own thread. */
  ytm_stream_follow(url, (int)sizeof url);
  if (player_start(url, g_queue[idx].seconds) != 0) {
    set_status(player_error()[0] ? player_error() : "Playback failed");
    toast(g_status);
    return -1;
  }
  g_player_ui = 1;
  set_status("");
  return 0;
}

/* Songs in a row that stopped on an error. Reset when one ends cleanly or the user picks. */
static int g_fail_streak;

/* Play idx, or the next rows after it when a song cannot be opened (removed, region
 * locked). Gives up after a few in a row so a dead list does not spin. */
static int play_from(int idx) {
  int tries;
  for (tries = 0; tries < 3 && idx < g_nqueue; tries++, idx++) {
    if (play_queue_index(idx) == 0) return 0;
  }
  return -1;
}

static void play_list(Track *list, int n, int idx) {
  if (n <= 0 || idx < 0 || idx >= n) return;
  g_queue_mix = 0;
  g_fail_streak = 0;
  if (n > YTM_TRACK_CAP) n = YTM_TRACK_CAP;
  memcpy(g_queue, list, (size_t)n * sizeof(Track));
  g_nqueue = n;
  play_from(idx);
}

static int queue_find(const char *id) {
  int i;
  if (!id || !id[0]) return -1;
  for (i = 0; i < g_nqueue; i++) {
    if (g_queue[i].id[0] && strcmp(g_queue[i].id, id) == 0) return i;
  }
  return -1;
}

/* Append the automix that follows id (ytmusicapi get_watch_playlist), skipping queued songs. */
static int queue_add_mix(const char *id) {
  char err[192];
  int c, i, added = 0;
  c = ytm_radio(id, g_scratch, YTM_TRACK_CAP, err, (int)sizeof err);
  if (c <= 0) return 0;
  for (i = 0; i < c && g_nqueue < YTM_TRACK_CAP; i++) {
    if (!g_scratch[i].id[0] || queue_find(g_scratch[i].id) >= 0) continue;
    g_queue[g_nqueue++] = g_scratch[i];
    added++;
  }
  return added;
}

/* Keep the current song and one before it, so a long mix can keep growing. */
static void queue_drop_played(void) {
  int drop = g_qindex - 1;
  int i;
  if (drop <= 0) return;
  for (i = drop; i < g_nqueue; i++) g_queue[i - drop] = g_queue[i];
  g_nqueue -= drop;
  g_qindex -= drop;
  g_qpick -= drop;
  if (g_qpick < 0) g_qpick = 0;
}

/* A song that is already queued just jumps to it. Any other song starts a new queue: that
 * song, then the automix YouTube Music builds from it (and from the account, when signed in
 * with the OAuth client). Add to queue and Play next never replace the queue. */
static void play_one(const Track *t) {
  int at;
  Track pick;
  if (!t || !t->id[0]) return;
  at = queue_find(t->id);
  if (at >= 0) {
    play_queue_index(at);
    return;
  }
  pick = *t;
  g_fail_streak = 0;
  g_queue[0] = pick;
  g_nqueue = 1;
  g_qpick = 0;
  g_queue_mix = 0;
  if (play_queue_index(0) == 0) {
    if (queue_add_mix(pick.id) > 0) {
      g_queue_mix = 1;
      paint();
    }
    return;
  }
  /* The song itself would not open. Its mix usually still does. */
  if (queue_add_mix(pick.id) > 0) {
    g_queue_mix = 1;
    play_from(1);
  }
}

static int queue_insert_new(const Track *t, int at) {
  int i;
  if (!t || !t->id[0]) return -1;
  if (queue_find(t->id) >= 0) return 0;
  if (g_nqueue >= YTM_TRACK_CAP) return -1;
  if (at < 0) at = g_nqueue;
  if (at > g_nqueue) at = g_nqueue;
  for (i = g_nqueue; i > at; i--) g_queue[i] = g_queue[i - 1];
  g_queue[at] = *t;
  g_nqueue++;
  if (g_qindex >= at) g_qindex++;
  if (g_qpick >= at) g_qpick++;
  return 1;
}

static void add_one_end(const Track *t) {
  int was;
  if (!t || !t->id[0]) return;
  if (queue_find(t->id) >= 0) {
    set_status("Already in the queue");
    return;
  }
  if (g_nqueue >= YTM_TRACK_CAP) {
    set_status("Queue is full");
    return;
  }
  was = g_nqueue;
  g_queue[g_nqueue++] = *t;
  if (was == 0) play_queue_index(0);
  else set_status("Added to queue");
}

static void play_next_one(const Track *t) {
  if (!t || !t->id[0]) return;
  if (queue_find(t->id) >= 0) {
    set_status("Already in the queue");
    return;
  }
  if (g_nqueue <= 0 || g_qindex < 0) {
    play_one(t);
    return;
  }
  if (queue_insert_new(t, g_qindex + 1) < 0) set_status("Queue is full");
  else set_status("Playing next");
}

static void maybe_advance(void) {
  if (!player_ended()) return;
  player_ack_ended();
  if (player_error()[0]) {
    /* A song that stops on an error skips ahead, up to a few in a row. */
    set_status(player_error());
    if (++g_fail_streak >= 3) return;
  } else {
    g_fail_streak = 0;
    if (g_repeat == REP_ONE && g_qindex >= 0) {
      player_replay();
      return;
    }
  }
  if (g_qindex + 1 >= g_nqueue && g_queue_mix && g_repeat == REP_OFF && g_qindex >= 0) {
    /* The mix ran out: continue it from the song that just ended. */
    if (g_nqueue >= YTM_TRACK_CAP) queue_drop_played();
    queue_add_mix(g_queue[g_qindex].id);
  }
  if (g_qindex + 1 < g_nqueue) play_from(g_qindex + 1);
  else if (g_repeat == REP_ALL && g_nqueue > 0) play_from(0);
}

static void open_shelf(int idx) {
  char err[192];
  int n;
  if (idx < 0 || idx > 2) return;
  if (g_body != BODY_LIST) remember_here();
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

static Track g_quick[24];
static Track g_mixes[16];
static int g_nquick;
static int g_nmixes;
static int g_quick_sel;
static int g_mix_sel;
static int g_home_sec;
static int g_home_try;
static int g_home_auth = -1;

static int home_ready(void) { return g_nquick > 0 || g_nmixes > 0; }

static void load_home(void) {
  char err[192];
  int ns = 0, nm = 0;
  int auth = ytm_signed_in();
  if (g_home_try && home_ready() && g_home_auth == auth) return;
  if (g_home_try && g_home_auth == auth && !auth) return;
  g_home_try = 1;
  g_home_auth = auth;
  set_status(auth ? "Loading your home..." : "Loading charts...");
  paint();
  if (ytm_home(g_quick, 24, &ns, g_mixes, 16, &nm, err, (int)sizeof err) != 0) {
    g_nquick = 0;
    g_nmixes = 0;
    set_status(err[0] ? err : "Pick a shelf.");
    return;
  }
  g_nquick = ns;
  g_nmixes = nm;
  g_quick_sel = 0;
  g_mix_sel = 0;
  g_home_sec = ns > 0 ? 0 : 1;
  set_status(ytm_home_note());
}

static int open_list(const char *browse, const char *title, int autoplay) {
  char err[192];
  int c;
  if (!browse || !browse[0]) return -1;
  if (g_body != BODY_LIST) remember_here();
  snprintf(g_list_title, sizeof g_list_title, "%s", title && title[0] ? title : "Songs");
  set_status("Opening...");
  paint();
  c = ytm_browse(browse, g_results, YTM_TRACK_CAP, err, (int)sizeof err);
  if (c < 0) {
    set_status(err[0] ? err : "Could not open that");
    toast(g_status);
    return -1;
  }
  g_nresults = c;
  g_sel = 0;
  g_scroll = 0;
  g_body = BODY_LIST;
  g_zone = ZONE_BODY;
  g_player_ui = 0;
  g_menu = 0;
  if (c <= 0) {
    set_status(err[0] ? err : "Nothing in that shelf");
    return 0;
  }
  if (autoplay) play_list(g_results, g_nresults, 0);
  else {
    char msg[64];
    snprintf(msg, sizeof msg, "%d songs", c);
    set_status(msg);
  }
  return c;
}

static void append_browse(const char *browse, int after_current) {
  char err[192];
  int c, i, added = 0;
  if (!browse || !browse[0]) return;
  set_status("Adding...");
  paint();
  c = ytm_browse(browse, g_scratch, YTM_TRACK_CAP, err, (int)sizeof err);
  if (c <= 0) {
    set_status(err[0] ? err : "Nothing to add");
    return;
  }
  for (i = 0; i < c; i++) {
    int at;
    if (!g_scratch[i].id[0]) continue;
    if (g_nqueue >= YTM_TRACK_CAP) break;
    if (after_current && g_nqueue > 0 && g_qindex >= 0)
      at = g_qindex + 1 + added;
    else
      at = g_nqueue;
    if (queue_insert_new(&g_scratch[i], at) <= 0) continue;
    added++;
  }
  if (added < 1) set_status(g_nqueue >= YTM_TRACK_CAP ? "Queue is full" : "Already in the queue");
  else if (g_qindex < 0) play_queue_index(0);
  else {
    char msg[48];
    snprintf(msg, sizeof msg, "Added %d", added);
    set_status(msg);
  }
}

static void play_or_open(Track *list, int n, int idx) {
  if (!list || idx < 0 || idx >= n) return;
  if (list[idx].id[0]) {
    play_one(&list[idx]);
    return;
  }
  open_list(list[idx].browse, list[idx].title, 1);
}

static Track *focused_track(void) {
  if (g_player_ui) {
    if (g_nqueue > 0 && g_qpick >= 0 && g_qpick < g_nqueue) return &g_queue[g_qpick];
    return NULL;
  }
  if (g_zone != ZONE_BODY) return NULL;
  if (g_body == BODY_HOME && home_ready()) {
    if (g_home_sec == 0 && g_quick_sel >= 0 && g_quick_sel < g_nquick) return &g_quick[g_quick_sel];
    if (g_home_sec == 1 && g_mix_sel >= 0 && g_mix_sel < g_nmixes) return &g_mixes[g_mix_sel];
    return NULL;
  }
  if (g_body == BODY_LIST && g_sel >= 0 && g_sel < g_nresults) return &g_results[g_sel];
  if (g_body == BODY_LIBRARY && g_sel >= 0 && g_sel < g_nlikes) return &g_likes[g_sel];
  return NULL;
}

static void menu_open(void) {
  Track *t = focused_track();
  int n = 0;
  if (!t || (!t->id[0] && !t->browse[0])) return;
  g_menu_label[n] = "Play";
  g_menu_act[n++] = 1;
  g_menu_label[n] = "Play next";
  g_menu_act[n++] = 2;
  g_menu_label[n] = "Add to queue";
  g_menu_act[n++] = 3;
  if (t->album_id[0] && strcmp(t->album_id, t->browse) != 0) {
    g_menu_label[n] = "Show album";
    g_menu_act[n++] = 4;
  }
  if (t->artist_id[0] && strcmp(t->artist_id, t->browse) != 0) {
    g_menu_label[n] = "Show artist";
    g_menu_act[n++] = 5;
  }
  g_menu_n = n;
  g_menu_sel = 0;
  g_menu = 1;
}

static void menu_do(void) {
  Track t;
  Track *cur;
  int act;
  if (!g_menu) return;
  cur = focused_track();
  if (!cur || g_menu_sel < 0 || g_menu_sel >= g_menu_n) {
    g_menu = 0;
    return;
  }
  t = *cur;
  act = g_menu_act[g_menu_sel];
  g_menu = 0;
  if (act == 1) {
    if (t.id[0]) play_one(&t);
    else open_list(t.browse, t.title, 1);
  } else if (act == 2) {
    if (t.id[0]) play_next_one(&t);
    else append_browse(t.browse, 1);
  } else if (act == 3) {
    if (t.id[0]) add_one_end(&t);
    else append_browse(t.browse, 0);
  } else if (act == 4) {
    open_list(t.album_id, t.album[0] ? t.album : "Album", 0);
  } else if (act == 5) {
    open_list(t.artist_id, t.artist[0] ? t.artist : "Artist", 0);
  }
}

static void on_activate(void) {
  if (g_menu) {
    menu_do();
    return;
  }
  if (g_player_ui) {
    if (g_nqueue > 0 && g_qpick >= 0 && g_qpick < g_nqueue && g_qpick != g_qindex)
      play_queue_index(g_qpick);
    else
      player_toggle();
    return;
  }
  if (g_zone == ZONE_NAV) {
    if (g_nav == NAV_QUIT) {
      g_run = 0;
      return;
    }
    if (g_nav == NAV_HOME) {
      g_body = BODY_HOME;
      if (!home_ready()) load_home();
    } else if (g_nav == NAV_SEARCH) {
      g_body = BODY_SEARCH;
      g_player_ui = 0;
      set_status("");
    } else if (g_nav == NAV_EXPLORE) {
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
    if (home_ready()) {
      if (g_home_sec == 0 && g_nquick > 0) play_or_open(g_quick, g_nquick, g_quick_sel);
      else if (g_nmixes > 0) play_or_open(g_mixes, g_nmixes, g_mix_sel);
    } else {
      do_search(moods[g_home][1], 0);
      if (g_nresults > 0) play_list(g_results, g_nresults, 0);
    }
  } else if (g_body == BODY_EXPLORE) {
    open_shelf(g_explore);
  } else if (g_body == BODY_LIST) {
    play_or_open(g_results, g_nresults, g_sel);
  } else if (g_body == BODY_LIBRARY) {
    if (!ytm_signed_in()) open_account();
    else if (g_nlikes > 0) play_or_open(g_likes, g_nlikes, g_sel);
    else open_library();
  } else if (g_body == BODY_ACCOUNT) {
    if (g_acct_sel == 1) {
      /* Nothing else may play while it runs. */
      player_stop();
      if (bgprobe_start() == 0) set_status("Background audio test running: go to the home screen");
      else set_status("The background audio test is already running");
      return;
    }
    if (!ytm_signed_in()) {
      start_code();
      return;
    }
    if (g_acct_sel == 0) {
      ytm_auth_signout();
      remove("/data/ytmusic/auth.txt");
      g_home_try = 0;
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
      do_search(g_query, 1);
    }
  }
}

static void on_back(void) {
  if (g_menu) {
    g_menu = 0;
    return;
  }
  if (g_player_ui) {
    g_player_ui = 0;
    return;
  }
  g_auth_wait = 0;
  if (g_nback > 0) {
    restore_here();
    return;
  }
  if (g_body != BODY_HOME || g_zone != ZONE_NAV) {
    g_body = BODY_HOME;
    g_zone = ZONE_NAV;
    g_nav = NAV_HOME;
    return;
  }
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
  if (g_menu) {
    if (dy) {
      g_menu_sel += dy;
      if (g_menu_sel < 0) g_menu_sel = 0;
      if (g_menu_sel >= g_menu_n) g_menu_sel = g_menu_n - 1;
    }
    return;
  }
  if (g_player_ui) {
    if (dx) player_seek_by(dx * 10.0);
    if (dy) {
      g_qpick += dy;
      if (g_qpick < 0) g_qpick = 0;
      if (g_nqueue > 0 && g_qpick >= g_nqueue) g_qpick = g_nqueue - 1;
    }
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
  if (dx < 0 && g_body != BODY_SEARCH && g_body != BODY_HOME) {
    g_zone = ZONE_NAV;
    return;
  }
  if (g_body == BODY_HOME && home_ready()) {
    int cols = 2;
    int *sel = g_home_sec == 0 ? &g_quick_sel : &g_mix_sel;
    int n = g_home_sec == 0 ? g_nquick : g_nmixes;
    int x, y, rows;
    if (n < 1 && g_home_sec == 0 && g_nmixes > 0) {
      g_home_sec = 1;
      sel = &g_mix_sel;
      n = g_nmixes;
    }
    if (n < 1) return;
    x = *sel % cols;
    y = *sel / cols;
    rows = (n + cols - 1) / cols;
    x += dx;
    y += dy;
    if (x < 0) {
      g_zone = ZONE_NAV;
      return;
    }
    if (x >= cols) x = cols - 1;
    if (y < 0) {
      if (g_home_sec == 1 && g_nquick > 0) {
        g_home_sec = 0;
        if (g_quick_sel >= g_nquick) g_quick_sel = g_nquick - 1;
      }
      return;
    }
    if (y >= rows) {
      if (g_home_sec == 0 && g_nmixes > 0) {
        g_home_sec = 1;
        if (g_mix_sel >= g_nmixes) g_mix_sel = g_nmixes - 1;
      }
      return;
    }
    if (y * cols + x >= n) x = (n - 1) % cols;
    *sel = y * cols + x;
    if (*sel >= n) *sel = n - 1;
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
    if (dy) g_acct_sel = dy > 0 ? 1 : 0;
    return;
  }
  g_sel += dy;
  keep_sel_visible(g_body == BODY_LIBRARY ? g_nlikes : g_nresults);
}

static int g_art_budget;

static void cover_rgb(const char *s, int *r, int *g, int *b) {
  static const int pal[][3] = {
      {147, 0, 10},   {93, 63, 60},  {120, 48, 72}, {32, 72, 88},
      {88, 48, 24},   {56, 48, 96},  {140, 56, 48}, {40, 80, 64},
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

static void stroke_seg(Draw *d, int x0, int y0, int x1, int y1, int rad, int r, int g, int b) {
  int dx = x1 - x0;
  int dy = y1 - y0;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  int i;
  if (steps < 1) steps = 1;
  for (i = 0; i <= steps; i++) draw_disc(d, x0 + dx * i / steps, y0 + dy * i / steps, rad, r, g, b);
}

static void tri_right(Draw *d, int tipx, int midy, int n, int r, int g, int b) {
  int i;
  for (i = 0; i < n; i++) fill_v(d, tipx - i, midy - i, 1, i * 2 + 1, r, g, b);
}

static void tri_left(Draw *d, int tipx, int midy, int n, int r, int g, int b) {
  int i;
  for (i = 0; i < n; i++) fill_v(d, tipx + i, midy - i, 1, i * 2 + 1, r, g, b);
}

static void draw_repeat_icon(Draw *d, int cx, int cy, int mode, int hr, int hg, int hb) {
  /* Light disc, dark ink. Rounded hooks, not a copied glyph. */
  static const int top_arc[][2] = {
      {-4, 0}, {-4, -1}, {-4, -2}, {-3, -2}, {-3, -3}, {-2, -3}, {-2, -4}, {-1, -4}, {0, -4},
  };
  static const int bot_arc[][2] = {
      {4, 0}, {4, 1}, {4, 2}, {3, 2}, {3, 3}, {2, 3}, {2, 4}, {1, 4}, {0, 4},
  };
  int i;
  int ink_r = 255, ink_g = 218, ink_b = 214;
  (void)hr;
  (void)hg;
  (void)hb;
  if (mode == REP_OFF) {
    ink_r = 215;
    ink_g = 194;
    ink_b = 191;
    draw_disc(d, cx, cy, 22, M3_SURF_HIGHEST);
  } else {
    draw_disc(d, cx, cy, 22, M3_PRIMARY_CTN);
  }
  stroke_seg(d, cx - 10, cy + 1, cx - 10, cy - 5, 1, ink_r, ink_g, ink_b);
  for (i = 0; i < 8; i++)
    stroke_seg(d, cx - 6 + top_arc[i][0], cy - 5 + top_arc[i][1], cx - 6 + top_arc[i + 1][0],
               cy - 5 + top_arc[i + 1][1], 1, ink_r, ink_g, ink_b);
  stroke_seg(d, cx - 6, cy - 9, cx + 7, cy - 9, 1, ink_r, ink_g, ink_b);
  tri_right(d, cx + 11, cy - 9, 5, ink_r, ink_g, ink_b);
  stroke_seg(d, cx + 10, cy - 1, cx + 10, cy + 5, 1, ink_r, ink_g, ink_b);
  for (i = 0; i < 8; i++)
    stroke_seg(d, cx + 6 + bot_arc[i][0], cy + 5 + bot_arc[i][1], cx + 6 + bot_arc[i + 1][0],
               cy + 5 + bot_arc[i + 1][1], 1, ink_r, ink_g, ink_b);
  stroke_seg(d, cx + 6, cy + 9, cx - 7, cy + 9, 1, ink_r, ink_g, ink_b);
  tri_left(d, cx - 11, cy + 9, 5, ink_r, ink_g, ink_b);
  if (mode == REP_ONE) {
    stroke_seg(d, cx - 2, cy - 1, cx, cy - 4, 1, ink_r, ink_g, ink_b);
    stroke_seg(d, cx, cy - 4, cx, cy + 4, 1, ink_r, ink_g, ink_b);
    stroke_seg(d, cx - 3, cy + 4, cx + 3, cy + 4, 1, ink_r, ink_g, ink_b);
  } else if (mode == REP_OFF) {
    stroke_seg(d, cx - 13, cy - 13, cx + 13, cy + 13, 1, ink_r, ink_g, ink_b);
  }
}

static void draw_volume(Draw *d, int x, int y, int w) {
  int vol = player_volume();
  int fw;
  int knob;
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  fill_round(d, x, y, w, 8, 4, M3_OUTLINE_VAR);
  fw = (w * vol) / 100;
  if (fw > 0) fill_round(d, x, y, fw, 8, 4, M3_PRIMARY);
  knob = x + fw;
  if (knob < x + 10) knob = x + 10;
  if (knob > x + w - 10) knob = x + w - 10;
  draw_disc(d, knob, y + 4, 11, M3_PRIMARY);
}

static void draw_cover(Draw *d, int x, int y, int size, const char *label, int ring) {
  int r, g, b;
  char letter[2];
  int tw;
  int rad = size > 200 ? 28 : 12;
  cover_rgb(label, &r, &g, &b);
  if (ring) fill_round(d, x - 4, y - 4, size + 8, size + 8, rad + 4, M3_PRIMARY);
  fill_round(d, x, y, size, size, rad, r, g, b);
  letter[0] = (label && label[0]) ? (char)toupper((unsigned char)label[0]) : 'M';
  letter[1] = 0;
  tw = text_px(letter, 1);
  draw_text(d, x + (size - tw) / 2, y + size / 2 - 18, 1, 255, 248, 236, letter);
}

static void draw_track_art(Draw *d, int x, int y, int size, const Track *t) {
  int w = 0, h = 0;
  const unsigned char *px;
  if (!t) return;
  px = ytm_cover_pixels(t, size, &w, &h);
  if (!px && g_art_budget > 0 && (t->id[0] || t->thumb[0] || t->browse[0])) {
    g_art_budget--;
    ytm_cover_fetch(t, size);
    px = ytm_cover_pixels(t, size, &w, &h);
  }
  if (px && w > 0 && h > 0) blit_cover(d, x, y, size, px, w, h);
  else draw_cover(d, x, y, size, t->title[0] ? t->title : "M", 0);
}

/* Artist line. Music videos and episodes get a chip first, so they read apart from songs
 * (YouTube Music musicVideoType: ATV is a song; OMV, UGC and OFFICIAL_SOURCE_MUSIC are videos). */
static void draw_byline(Draw *d, char *line, int line_n, int x, int y, int max_px, const Track *t) {
  const char *tag = NULL;
  if (!t) return;
  if (t->kind == YTM_KIND_VIDEO) tag = "Video";
  else if (t->kind == YTM_KIND_EPISODE) tag = "Episode";
  if (tag) {
    int cw = text_px(tag, 1) + 20;
    fill_round(d, x, y - 3, cw, 34, 8, M3_SECONDARY_CTN);
    draw_text(d, x + 10, y, 1, M3_ON_SECONDARY_CTN, tag);
    x += cw + 12;
    max_px -= cw + 12;
  }
  if (max_px < 40) return;
  fit(line, line_n, t->artist, 1, max_px);
  draw_text(d, x, y, 1, M3_ON_SURFACE_VAR, line);
}

static void draw_menu(Draw *d) {
  Track *t;
  char line[120];
  int i, y, h;
  const int x = 560;
  const int w = 800;
  const int row = 68;
  if (!g_menu || g_menu_n < 1) return;
  t = focused_track();
  h = 96 + g_menu_n * row;
  y = (980 - h) / 2;
  if (y < 36) y = 36;
  fill_round(d, x, y, w, h, 8, M3_SURF_HIGH);
  fit(line, (int)sizeof line, t && t->title[0] ? t->title : "Options", 1, w - 64);
  draw_text(d, x + 32, y + 22, 1, M3_ON_SURFACE_VAR, line);
  for (i = 0; i < g_menu_n; i++) {
    int yy = y + 72 + i * row;
    if (i == g_menu_sel) fill_round(d, x + 12, yy - 6, w - 24, row - 10, 8, M3_SECONDARY_CTN);
    draw_text(d, x + 40, yy + 8, 1, i == g_menu_sel ? 255 : 240, i == g_menu_sel ? 218 : 222,
              i == g_menu_sel ? 214 : 220, g_menu_label[i]);
  }
}

static void present(Draw *d) {
  static int told;
  draw_menu(d);
  draw_end(d);
  if (SDL_UpdateWindowSurface(g_win) == 0) return;
  if (told) return;
  told = 1;
  toast(SDL_GetError());
}

static void draw_hints(Draw *d) {
  const char *s;
  fill_v(d, 0, HINT_Y, 1920, 1080 - HINT_Y, M3_SURF_LOWEST);
  fill_v(d, 0, HINT_Y, 1920, 1, M3_OUTLINE_VAR);
  if (g_menu) {
    s = "X Choose   O Close   D-pad Move   Square Close";
  } else if (g_player_ui) {
    s = "X Play   Square Menu   O Back   D-pad Queue   L2 R2 Volume   L1 R1 Skip   Touchpad Repeat";
  } else if (g_body == BODY_ACCOUNT) {
    s = ytm_signed_in()
            ? "X Sign out   O Back   Options Pause   Touchpad Repeat"
            : "X New code   O Back   Options Pause";
  } else if (g_body == BODY_SEARCH) {
    s = "X Type   Square Delete   O Back   D-pad Move   Options Pause";
  } else if (g_body == BODY_HOME) {
    s = "X Play   Square Menu   O Back   D-pad Move   Options Pause   Triangle Player";
  } else if (g_body == BODY_EXPLORE) {
    s = "X Open   O Back   D-pad Move   Options Pause   Touchpad Repeat";
  } else if (g_body == BODY_LIBRARY) {
    s = ytm_signed_in()
            ? "X Play   Square Menu   O Back   D-pad Move   Options Pause   Triangle Player"
            : "X Sign in   O Back   Options Pause   Touchpad Repeat";
  } else {
    s = "X Play   Square Menu   O Back   D-pad Move   Triangle Player   Options Pause";
  }
  draw_text(d, 28, HINT_Y + 16, 1, M3_ON_SURFACE_VAR, s);
}

/* first_row is the first visible row of a 2-column grid. Returns y after the last row. */
static int draw_two_col(Draw *d, char *line, int line_n, const Track *items, int n,
                        int sel_idx, int selected, int first_row, int x0, int y, int rows) {
  const int colw = 760;
  const int x1 = x0 + colw + 16;
  int r, c;
  for (r = 0; r < rows; r++) {
    for (c = 0; c < 2; c++) {
      int idx = (first_row + r) * 2 + c;
      int x = c ? x1 : x0;
      int yy = y + r * 74;
      if (idx >= n) continue;
      if (selected && sel_idx == idx) fill_round(d, x - 8, yy - 4, colw, 68, 12, M3_SECONDARY_CTN);
      draw_track_art(d, x, yy, 56, &items[idx]);
      fit(line, line_n, items[idx].title, 1, 640);
      draw_text(d, x + 68, yy + 2, 1, M3_ON_SURFACE, line);
      draw_byline(d, line, line_n, x + 68, yy + 30, 400, &items[idx]);
    }
  }
  return y + rows * 74;
}

static int window_row(int sel, int n, int rows_vis) {
  int row = sel / 2;
  int total = (n + 1) / 2;
  int first = 0;
  if (rows_vis < 1) rows_vis = 1;
  if (row >= rows_vis) first = row - rows_vis + 1;
  if (first + rows_vis > total) first = total - rows_vis;
  if (first < 0) first = 0;
  return first;
}

static void paint(void) {
  Draw d;
  char line[180];
  SDL_Surface *surf = SDL_GetWindowSurface(g_win);
  const Track *now = (g_qindex >= 0 && g_qindex < g_nqueue) ? &g_queue[g_qindex] : NULL;
  if (!surf) return;
  g_art_budget = 2;
  if (now && (now->id[0] || now->thumb[0]) && !ytm_cover_pixels(now, 280, NULL, NULL)) {
    ytm_cover_fetch(now, 280);
    g_art_budget--;
  }
  draw_begin(&d, surf);
  fill_v(&d, 0, 0, 1920, 1080, M3_SURFACE);

  if (g_player_ui && now) {
    char a[16], b[16];
    int dur = (int)player_duration();
    int pos = (int)player_position();
    int w = 0;
    int qvis = 8;
    int qscroll = 0;
    int chip;
    if (dur < 1) dur = now->seconds;
    if (g_qpick >= qvis) qscroll = g_qpick - qvis + 1;
    fill_v(&d, 0, 0, 1920, 1080, M3_SURFACE);
    fill_round(&d, 24, 20, 1872, HINT_Y - 40, 28, M3_SURF_CTN);
    chip = text_px("Now playing", 1) + 32;
    fill_round(&d, 56, 44, chip, 36, 18, M3_PRIMARY_CTN);
    draw_text(&d, 72, 50, 1, M3_ON_PRIMARY_CTN, "Now playing");
    draw_track_art(&d, 72, 112, 280, now);
    fit(line, (int)sizeof line, now->title, 2, 860);
    draw_text(&d, 72, 412, 2, M3_ON_SURFACE, line);
    draw_byline(&d, line, (int)sizeof line, 72, 482, 860, now);
    if (now->album[0]) {
      fit(line, (int)sizeof line, now->album, 1, 860);
      draw_text(&d, 72, 522, 1, M3_OUTLINE, line);
    }
    fill_round(&d, 72, 592, 860, 8, 4, M3_OUTLINE_VAR);
    if (dur > 0) {
      w = (int)(860.0 * (pos / (double)dur));
      if (w < 0) w = 0;
      if (w > 860) w = 860;
      if (w > 0) fill_round(&d, 72, 592, w, 8, 4, M3_PRIMARY);
    }
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, 72, 610, 1, M3_ON_SURFACE_VAR, a);
    draw_text(&d, 932 - text_px(b, 1), 610, 1, M3_ON_SURFACE_VAR, b);
    draw_text(&d, 72, 672, 1, M3_ON_SURFACE_VAR, "Volume");
    draw_volume(&d, 220, 684, 480);
    draw_repeat_icon(&d, 800, 688, g_repeat, M3_SURF_CTN);
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 860);
      draw_text(&d, 72, 752, 1, M3_ERROR, line);
    } else if (player_error()[0]) {
      fit(line, (int)sizeof line, player_error(), 1, 860);
      draw_text(&d, 72, 752, 1, M3_ERROR, line);
    } else if (player_paused()) {
      draw_text(&d, 72, 752, 1, M3_ON_SURFACE_VAR, "Paused");
    }
    fill_round(&d, 980, 100, 860, 770, 16, M3_SURF_HIGH);
    draw_text(&d, 1004, 120, 1, M3_ON_SURFACE, "Up next");
    for (int i = 0; i < qvis && qscroll + i < g_nqueue; i++) {
      int idx = qscroll + i;
      int y = 172 + i * 82;
      int on = idx == g_qpick;
      if (on) fill_round(&d, 996, y - 6, 828, 76, 12, M3_SECONDARY_CTN);
      if (idx == g_qindex) fill_v(&d, 996, y, 4, 64, M3_PRIMARY);
      draw_track_art(&d, 1016, y, 64, &g_queue[idx]);
      fit(line, (int)sizeof line, g_queue[idx].title, 1, 680);
      draw_text(&d, 1096, y + 4, 1, M3_ON_SURFACE, line);
      draw_byline(&d, line, (int)sizeof line, 1096, y + 36, 480, &g_queue[idx]);
    }
    draw_hints(&d);
    present(&d);
    return;
  }

  fill_v(&d, 0, 0, RAIL, BAR_Y, M3_SURF_LOW);
  fill_v(&d, RAIL - 1, 0, 1, BAR_Y, M3_OUTLINE_VAR);
  draw_disc(&d, 40, 52, 12, M3_PRIMARY);
  draw_text(&d, 64, 22, 1, M3_ON_SURFACE, "YouTube");
  draw_text(&d, 64, 54, 1, M3_ON_SURFACE_VAR, "Music");
  {
    const char *mode = "Choosing a section";
    if (g_zone != ZONE_NAV) {
      if (g_body == BODY_HOME) mode = home_ready() ? "For you" : "Shelves";
      else if (g_body == BODY_EXPLORE) mode = "Explore";
      else if (g_body == BODY_SEARCH) mode = "Search";
      else if (g_body == BODY_LIBRARY) mode = "Your library";
      else if (g_body == BODY_ACCOUNT) mode = ytm_signed_in() ? "Signed in" : "Signing in";
      else mode = "Song list";
    }
    draw_text(&d, 28, 104, 1, M3_PRIMARY, mode);
  }

  for (int i = 0; i < NAV_QUIT; i++) {
    int y = 156 + i * 68;
    int on = (g_nav == i);
    int nr = 215, ng = 194, nb = 191;
    if (on) {
      int focus = (g_zone == ZONE_NAV);
      fill_round(&d, 12, y - 8, RAIL - 24, 48, 24, focus ? 147 : 93, focus ? 0 : 63, focus ? 10 : 60);
      nr = 255;
      ng = 218;
      nb = 214;
    }
    draw_text(&d, 36, y, 1, nr, ng, nb, nav_name[i]);
  }
  {
    int on = (g_nav == NAV_QUIT);
    int nr = 160, ng = 140, nb = 137;
    if (on) {
      int focus = (g_zone == ZONE_NAV);
      fill_round(&d, 12, BAR_Y - 80, RAIL - 24, 48, 24, focus ? 147 : 93, focus ? 0 : 63,
                 focus ? 10 : 60);
      nr = 255;
      ng = 218;
      nb = 214;
    }
    draw_text(&d, 36, BAR_Y - 68, 1, nr, ng, nb, "Quit");
  }

  if (g_body == BODY_HOME) {
    if (home_ready()) {
      int x0 = RAIL + 28;
      int y = 104;
      int focus = (g_zone == ZONE_BODY);
      const char *place = ytm_home_place();
      if ((!ytm_signed_in() || ytm_home_note()[0]) && place && place[0]) {
        snprintf(line, sizeof line, "Charts · %s", place);
        draw_text(&d, x0, 16, 2, M3_ON_SURFACE, line);
      } else {
        draw_text(&d, x0, 16, 2, M3_ON_SURFACE, "For you");
      }
      if (g_status[0]) {
        fit(line, (int)sizeof line, g_status, 1, 1000);
        draw_text(&d, 1920 - 40 - text_px(line, 1), 28, 1, M3_ERROR, line);
      }
      draw_text(&d, x0, 72, 1, M3_PRIMARY, ytm_home_song_heading());
      if (g_nquick < 1) {
        draw_text(&d, x0, y, 1, M3_ON_SURFACE_VAR, "No songs yet");
        y += 40;
      } else {
        int cap = g_nmixes > 0 ? 5 : (888 - y) / 74;
        int total = (g_nquick + 1) / 2;
        if (cap > total) cap = total;
        if (cap < 1) cap = 1;
        y = draw_two_col(&d, line, (int)sizeof line, g_quick, g_nquick, g_quick_sel,
                         focus && g_home_sec == 0,
                         (focus && g_home_sec == 0) ? window_row(g_quick_sel, g_nquick, cap) : 0,
                         x0, y, cap);
      }
      y += 8;
      draw_text(&d, x0, y, 1, M3_PRIMARY, ytm_home_mix_heading());
      y += 36;
      if (g_nmixes < 1) {
        draw_text(&d, x0, y, 1, M3_ON_SURFACE_VAR, "No playlists yet");
      } else {
        int cap = (888 - y) / 74;
        int total = (g_nmixes + 1) / 2;
        if (cap > total) cap = total;
        if (cap < 1) cap = 1;
        draw_two_col(&d, line, (int)sizeof line, g_mixes, g_nmixes, g_mix_sel,
                     focus && g_home_sec == 1,
                     (focus && g_home_sec == 1) ? window_row(g_mix_sel, g_nmixes, cap) : 0,
                     x0, y, cap);
      }
    } else {
    draw_text(&d, RAIL + 28, 16, 2, M3_ON_SURFACE, "Shelves");
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, RAIL + 28, 72, 1, M3_ERROR, line);
    }
    for (int i = 0; i < 8; i++) {
      int col = i % 4;
      int row = i / 4;
      int x = RAIL + 28 + col * 390;
      int y = 140 + row * 250;
      int sel = (g_zone == ZONE_BODY && g_home == i);
      int cr, cg, cb;
      cover_rgb(moods[i][0], &cr, &cg, &cb);
      if (sel) fill_round(&d, x - 3, y - 3, 366, 206, 16, M3_PRIMARY);
      fill_round(&d, x, y, 360, 200, 12, M3_SURF_HIGH);
      fill_round(&d, x + 24, y + 36, 72, 8, 4, cr, cg, cb);
      draw_text(&d, x + 24, y + 64, 2, M3_ON_SURFACE, moods[i][0]);
    }
    }
  } else if (g_body == BODY_EXPLORE) {
    draw_text(&d, RAIL + 28, 16, 2, M3_ON_SURFACE, "Explore");
    draw_text(&d, RAIL + 28, 80, 1, M3_ON_SURFACE_VAR, "New music, charts, and what is playing now");
    for (int i = 0; i < 3; i++) {
      int y = 150 + i * 220;
      int sel = (g_zone == ZONE_BODY && g_explore == i);
      int cr, cg, cb;
      cover_rgb(explore_shelf[i][0], &cr, &cg, &cb);
      if (sel) fill_round(&d, RAIL + 24, y - 3, 1540, 194, 16, M3_PRIMARY);
      fill_round(&d, RAIL + 28, y, 1532, 188, 12, M3_SURF_HIGH);
      fill_v(&d, RAIL + 28, y + 28, 8, 132, cr, cg, cb);
      draw_text(&d, RAIL + 64, y + 48, 2, M3_ON_SURFACE, explore_shelf[i][0]);
      draw_text(&d, RAIL + 64, y + 118, 1, M3_ON_SURFACE_VAR, explore_shelf[i][1]);
    }
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, RAIL + 28, 830, 1, M3_ERROR, line);
    }
  } else if (g_body == BODY_SEARCH) {
    fill_round(&d, RAIL + 28, 20, 1560, 56, 28, M3_SURF_HIGH);
    fit(line, (int)sizeof line, g_query[0] ? g_query : "Search songs, albums, artists", 1, 1500);
    draw_text(&d, RAIL + 52, 34, 1, g_query[0] ? 240 : 160, g_query[0] ? 222 : 140, g_query[0] ? 220 : 137,
              line);
    for (int r = 0; r < 5; r++) {
      int cols = key_cols(r);
      int indent = r == 2 ? 48 : r == 3 ? 96 : 0;
      for (int c = 0; c < cols; c++) {
        int x = RAIL + 28 + indent + c * (r == 4 ? 280 : 118);
        int y = 112 + r * 100;
        int sel = (g_zone == ZONE_BODY && g_keyr == r && g_keyc == c);
        char lab[8];
        int kw = r == 4 ? 250 : 104;
        fill_round(&d, x, y, kw, 80, sel ? 40 : 8, sel ? 147 : 57, sel ? 0 : 51, sel ? 10 : 50);
        if (r < 4) {
          lab[0] = krow[r][c];
          lab[1] = 0;
          draw_text(&d, x + (kw - text_px(lab, 1)) / 2, y + 22, 1, sel ? 255 : 240, sel ? 218 : 222,
                    sel ? 214 : 220, lab);
        } else {
          draw_text(&d, x + 28, y + 22, 1, sel ? 255 : 240, sel ? 218 : 222, sel ? 214 : 220,
                    specials[c]);
        }
      }
    }
    if (g_status[0]) {
      fit(line, (int)sizeof line, g_status, 1, 1400);
      draw_text(&d, RAIL + 28, 640, 1, M3_ERROR, line);
    }
  } else if (g_body == BODY_ACCOUNT) {
    const char *shown = g_verify_url;
    snprintf(line, sizeof line, "Background player: %s", ytmd_status());
    draw_text(&d, RAIL + 28, 840, 1, M3_OUTLINE, line);
    draw_text(&d, RAIL + 28, 16, 2, M3_ON_SURFACE, ytm_signed_in() ? "Account" : "Sign in");
    if (!ytm_signed_in()) {
      if (strncmp(shown, "https://", 8) == 0) shown += 8;
      else if (strncmp(shown, "http://", 7) == 0) shown += 7;
      draw_text(&d, RAIL + 28, 100, 1, M3_ON_SURFACE_VAR, "On a phone or computer, open");
      draw_text(&d, RAIL + 28, 144, 1, M3_PRIMARY, shown[0] ? shown : "google.com/device");
      draw_text(&d, RAIL + 28, 200, 1, M3_ON_SURFACE_VAR, "and enter this code");
      fill_round(&d, RAIL + 28, 252, 560, 88, 12, M3_SURF_HIGH);
      draw_text(&d, RAIL + 52, 276, 2, M3_ON_SURFACE, g_user_code[0] ? g_user_code : "...");
      if (g_status[0]) {
        fit(line, (int)sizeof line, g_status, 1, 1400);
        draw_text(&d, RAIL + 28, 368, 1, M3_ERROR, line);
      }
      draw_text(&d, RAIL + 28, 440, 1, M3_ON_SURFACE_VAR, "X asks for a new code if this one expires");
    } else {
      const char *rows[] = {"Sign out"};
      draw_text(&d, RAIL + 28, 108, 1, M3_ON_SURFACE_VAR, "YouTube Music is linked to this console");
      {
        const char *how;
        if (ytm_token_custom())
          how = "Signed in with your OAuth client. Home shows your recommendations.";
        else if (ytm_oauth_client_set())
          how = "Sign out and in again to use your OAuth client for Home.";
        else
          how = "Home is public picks. Add oauth_client.txt for your own (see README).";
        draw_text(&d, RAIL + 28, 260, 1, M3_OUTLINE, how);
      }
      {
        int y = 180;
        int sel = (g_zone == ZONE_BODY && g_acct_sel == 0);
        fill_round(&d, RAIL + 28, y, 280, 56, 28, sel ? 147 : 57, sel ? 0 : 51, sel ? 10 : 50);
        draw_text(&d, RAIL + 52, y + 14, 1, sel ? 255 : 240, sel ? 218 : 222, sel ? 214 : 220, rows[0]);
      }
    }
    {
      /* Down to reach it, in both states. */
      int y = 740;
      int sel = (g_zone == ZONE_BODY && g_acct_sel == 1);
      const char *label = bgprobe_running() ? "Background audio test running" : "Background audio test";
      int w = text_px(label, 1) + 56;
      fill_round(&d, RAIL + 28, y, w, 56, 28, sel ? 147 : 57, sel ? 0 : 51, sel ? 10 : 50);
      draw_text(&d, RAIL + 56, y + 14, 1, sel ? 255 : 240, sel ? 218 : 222, sel ? 214 : 220, label);
    }
  } else {
    Track *list = g_body == BODY_LIBRARY ? g_likes : g_results;
    int n = g_body == BODY_LIBRARY ? g_nlikes : g_nresults;
    const char *heading = g_body == BODY_LIBRARY ? "Library"
                          : (g_list_title[0] ? g_list_title : "Songs");
    const int vis = 6;
    fit(line, (int)sizeof line, heading, 2, 1200);
    draw_text(&d, RAIL + 28, 16, 2, M3_ON_SURFACE, line);
    if (n == 0) {
      const char *empty = "No songs for that search.";
      if (g_status[0]) empty = g_status;
      else if (g_body == BODY_LIBRARY)
        empty = ytm_signed_in() ? "No liked songs on this account."
                                : "Sign in from Account to load your library.";
      fit(line, (int)sizeof line, empty, 1, 1400);
      draw_text(&d, RAIL + 28, 120, 1, M3_ON_SURFACE_VAR, line);
    }
    for (int i = 0; i < vis && g_scroll + i < n; i++) {
      int idx = g_scroll + i;
      int y = 112 + i * 120;
      int sel = (g_zone == ZONE_BODY && g_sel == idx);
      char time[16];
      if (sel) fill_round(&d, RAIL + 16, y - 8, 1588, 108, 12, M3_SECONDARY_CTN);
      draw_track_art(&d, RAIL + 36, y, 84, &list[idx]);
      fit(line, (int)sizeof line, list[idx].title, 1, 1100);
      draw_text(&d, RAIL + 140, y + 8, 1, M3_ON_SURFACE, line);
      draw_byline(&d, line, (int)sizeof line, RAIL + 140, y + 46, 900, &list[idx]);
      if (list[idx].seconds > 0) {
        fmt_time(time, (int)sizeof time, list[idx].seconds);
        draw_text(&d, RAIL + 1420, y + 24, 1, M3_ON_SURFACE_VAR, time);
      }
    }
  }

  fill_round(&d, 16, BAR_Y + 8, 1888, HINT_Y - BAR_Y - 16, 16, M3_SURF_HIGH);
  if (now) {
    char a[16], b[16];
    const char *sub = now->album[0] ? now->album : now->artist;
    int dur = (int)player_duration();
    int pos = (int)player_position();
    int bar_x = 700;
    int bar_w = 720;
    if (dur < 1) dur = now->seconds;
    draw_track_art(&d, 36, BAR_Y + 18, 72, now);
    fit(line, (int)sizeof line, now->title, 1, 400);
    draw_text(&d, 124, BAR_Y + 20, 1, M3_ON_SURFACE, line);
    if (now->kind == YTM_KIND_VIDEO && sub[0])
      snprintf(line, sizeof line, "Video · %s", sub);
    else if (now->kind == YTM_KIND_VIDEO)
      snprintf(line, sizeof line, "Video");
    else
      snprintf(line, sizeof line, "%s", sub);
    fit(line, (int)sizeof line, line, 1, 400);
    draw_text(&d, 124, BAR_Y + 54, 1, M3_ON_SURFACE_VAR, line);
    fmt_time(a, (int)sizeof a, pos);
    fmt_time(b, (int)sizeof b, dur);
    draw_text(&d, bar_x - 16 - text_px(a, 1), BAR_Y + 38, 1, M3_ON_SURFACE_VAR, a);
    fill_round(&d, bar_x, BAR_Y + 50, bar_w, 8, 4, M3_OUTLINE_VAR);
    if (dur > 0) {
      int bw = (int)((double)bar_w * (pos / (double)dur));
      if (bw < 0) bw = 0;
      if (bw > bar_w) bw = bar_w;
      if (bw > 0) fill_round(&d, bar_x, BAR_Y + 50, bw, 8, 4, M3_PRIMARY);
    }
    draw_text(&d, bar_x + bar_w + 16, BAR_Y + 38, 1, M3_ON_SURFACE_VAR, b);
    if (g_nqueue > 1) {
      char qn[24];
      snprintf(qn, sizeof qn, "%d up next", g_nqueue - (g_qindex + 1));
      if (g_qindex + 1 >= g_nqueue) snprintf(qn, sizeof qn, "End of queue");
      draw_text(&d, 1560, BAR_Y + 22, 1, M3_ON_SURFACE_VAR, qn);
    }
    draw_repeat_icon(&d, 1816, BAR_Y + 54, g_repeat, M3_SURF_HIGH);
  } else {
    draw_text(&d, 36, BAR_Y + 36, 1, M3_ON_SURFACE_VAR, "Nothing playing");
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
    if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) {
      g_pad = SDL_GameControllerOpen(e.cdevice.which);
      memset(g_prev, 0, sizeof g_prev);
    }
    /* The DualSense sleeps or loses power; keep the dead handle and the next pad is ignored. */
    if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
        e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
      SDL_GameControllerClose(g_pad);
      g_pad = NULL;
      g_hold = 0;
    }
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
  if (g_pad && !SDL_GameControllerGetAttached(g_pad)) {
    SDL_GameControllerClose(g_pad);
    g_pad = NULL;
    g_hold = 0;
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
  if (edge(SDL_CONTROLLER_BUTTON_X)) {
    if (g_body == BODY_SEARCH && !g_player_ui && !g_menu) {
      int n = (int)strlen(g_query);
      if (n > 0) g_query[n - 1] = 0;
    } else if (g_menu)
      g_menu = 0;
    else
      menu_open();
  }
  if (edge(SDL_CONTROLLER_BUTTON_Y)) {
    g_menu = 0;
    g_player_ui = !g_player_ui;
    if (g_player_ui) g_qpick = g_qindex >= 0 ? g_qindex : 0;
  }
  /* Options pauses or resumes. It does nothing until a song is actually playing. */
  if (edge(SDL_CONTROLLER_BUTTON_BACK)) player_toggle();
  if (edge(SDL_CONTROLLER_BUTTON_START)) {
    g_repeat = (g_repeat + 1) % 3;
    set_status(g_repeat == REP_ONE ? "Repeat one" : g_repeat == REP_ALL ? "Repeat all" : "Repeat off");
  }
  if (!g_menu && edge(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
    if (g_qindex > 0) play_queue_index(g_qindex - 1);
    else if (g_qindex == 0) player_seek_by(-1e9);
  }
  if (!g_menu && edge(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) && g_qindex + 1 < g_nqueue)
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
  if (g_player_ui) {
    int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (lt > 16000 && g_trig_l <= 16000) player_volume_add(-5);
    if (rt > 16000 && g_trig_r <= 16000) player_volume_add(5);
    g_trig_l = lt;
    g_trig_r = rt;
  } else {
    g_trig_l = 0;
    g_trig_r = 0;
  }
  hold_dir(dx, dy);
}

int main(int argc, char **argv) {
  char err[192];
  (void)argc;
  (void)argv;
  /* A peer that went away (the daemon, YouTube) is an error return, not the end of the app. */
  signal(SIGPIPE, SIG_IGN);
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
  /* The background player is optional: without an ELF loader the app plays as before. */
  if (!g_status[0]) {
    set_status("Starting the background player...");
    paint();
  }
  ytmd_start();
  if (strncmp(g_status, "Starting the background", 23) == 0) set_status("");
  {
    int net_ok = net_init(err, (int)sizeof err) == 0;
    if (!net_ok) {
      set_status(err);
      toast(err);
    } else if (!g_status[0]) {
      set_status("Pick a shelf.");
    }
    if (net_ok) {
      auth_load();
      load_home();
    }
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
        g_home_try = 0;
        g_nquick = 0;
        g_nmixes = 0;
        load_home();
      } else if (rc < 0) {
        g_auth_wait = 0;
        set_status(aerr);
        toast(aerr);
      }
    }
    maybe_advance();
    ytmd_tick();
    paint();
    SDL_Delay(16);
  }

  /* Quit stops the background player. A force close never gets here; the daemon notices
   * its connection close and exits by itself. */
  ytmd_stop();
  player_close();
  net_shutdown();
  if (g_pad) SDL_GameControllerClose(g_pad);
  SDL_DestroyWindow(g_win);
  SDL_Quit();
  return 0;
}
