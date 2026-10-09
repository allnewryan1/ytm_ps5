#include "net.h"
#include "art.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Prototypes match ps5-payload-dev/sdk samples/http2_get plus the
 * sceHttp2AddRequestHeader entry the stub exports (same shape as sceHttp). */
int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2Term(int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, unsigned long long);
int sceHttp2DeleteRequest(int);
int sceHttp2AddRequestHeader(int, const char *, const char *, unsigned int);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2ReadData(int, void *, size_t);
int sceHttp2SetAutoRedirect(int, int);
int sceHttp2GetAllResponseHeaders(int, char **, size_t *);

#define SCE_HTTP_HEADER_OVERWRITE 0u

/* Public web innertube key shipped in the YouTube Music page itself. */
static const char *INNERTUBE_KEY = "AIzaSyC9XL3ZjWddXya6X74dJoCTL-WEYFDNX30";

#define RESP_MAX (1536 * 1024)

static int g_net = -1;
static int g_ssl = -1;
static int g_http = -1;
static int g_tmpl = -1;
static char *g_resp;

#define GRID_SIDE 128
#define GRID_SLOTS 96
#define HERO_SIDE 360
#define HERO_SLOTS 3
typedef struct {
  char key[80];
  unsigned char *px;
  unsigned stamp;
  int fails;
} CoverSlot;
static CoverSlot g_grid[GRID_SLOTS];
static CoverSlot g_hero[HERO_SLOTS];
static unsigned g_grid_tick;
static unsigned g_hero_tick;

static void cover_pool_clear(CoverSlot *slots, int n) {
  int i;
  for (i = 0; i < n; i++) {
    free(slots[i].px);
    slots[i].px = NULL;
    slots[i].key[0] = 0;
    slots[i].stamp = 0;
    slots[i].fails = 0;
  }
}

static void cover_clear(void) {
  cover_pool_clear(g_grid, GRID_SLOTS);
  cover_pool_clear(g_hero, HERO_SLOTS);
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void emit_cp(char *dst, int n, int *o, unsigned cp) {
  if (cp < 32) cp = ' ';
  if (cp == 0xA0) cp = ' ';
  if (cp < 0x80) {
    if (*o + 1 < n) dst[(*o)++] = (char)cp;
  } else if (cp < 0x800) {
    if (*o + 2 < n) {
      dst[(*o)++] = (char)(0xC0 | (cp >> 6));
      dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
  } else if (cp < 0x10000) {
    if (*o + 3 < n) {
      dst[(*o)++] = (char)(0xE0 | (cp >> 12));
      dst[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
  } else if (cp <= 0x10FFFF) {
    if (*o + 4 < n) {
      dst[(*o)++] = (char)(0xF0 | (cp >> 18));
      dst[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      dst[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      dst[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
  }
}

static int decode_u(const char *s, unsigned *cp) {
  int h1, h2, h3, h4;
  if (s[0] != '\\' || s[1] != 'u') return 0;
  h1 = hex_nibble(s[2]);
  h2 = hex_nibble(s[3]);
  h3 = hex_nibble(s[4]);
  h4 = hex_nibble(s[5]);
  if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return 0;
  *cp = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
  return 6;
}

static void copy_json_str(const char *src, char *dst, int n) {
  int o = 0;
  if (n <= 0) return;
  while (*src && *src != '"') {
    if (*src == '\\') {
      unsigned cp = 0;
      int used = decode_u(src, &cp);
      if (used) {
        src += used;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          unsigned lo = 0;
          int u2 = decode_u(src, &lo);
          if (u2 && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
            src += u2;
          } else {
            continue;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          continue;
        }
        if (cp == 0) continue;
        emit_cp(dst, n, &o, cp);
        continue;
      }
      src++;
      if (*src == 0) break;
      if (*src == 'n' || *src == 't' || *src == 'r') {
        emit_cp(dst, n, &o, ' ');
        src++;
        continue;
      }
      emit_cp(dst, n, &o, (unsigned char)*src);
      src++;
      continue;
    }
    {
      unsigned char ch = (unsigned char)*src++;
      if (ch < 32) ch = ' ';
      if (o + 1 < n) dst[o++] = (char)ch;
    }
  }
  dst[o] = 0;
}

static int json_string(const char *json, const char *key, char *dst, int n) {
  char pat[96];
  const char *p;
  snprintf(pat, sizeof pat, "\"%s\":\"", key);
  p = strstr(json, pat);
  if (!p) {
    snprintf(pat, sizeof pat, "\"%s\": \"", key);
    p = strstr(json, pat);
  }
  if (!p) {
    dst[0] = 0;
    return 0;
  }
  copy_json_str(p + strlen(pat), dst, n);
  return dst[0] != 0;
}

static int json_int(const char *json, const char *key, int *out) {
  char pat[64];
  const char *p;
  snprintf(pat, sizeof pat, "\"%s\":", key);
  p = strstr(json, pat);
  if (!p) return 0;
  p += strlen(pat);
  while (*p == ' ' || *p == '"') p++;
  *out = atoi(p);
  return 1;
}

static void json_escape(const char *src, char *dst, int n) {
  int o = 0;
  for (int i = 0; src[i] && o + 2 < n; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '"' || c == '\\') {
      dst[o++] = '\\';
      dst[o++] = (char)c;
    } else if (c >= 32 && c < 127) {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
}

static int id_ok(const char *id) {
  if (strlen(id) != 11) return 0;
  for (int i = 0; i < 11; i++) {
    char c = id[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return 0;
  }
  return 1;
}

static int junk_label(const char *s) {
  static const char *bad[] = {
      "SONG", "Song", "Songs", "ALBUM", "Album", "ARTIST", "Artist",
      "Explicit", "MUSIC", "Music", "Watch", "Shuffle", "Play", "plays",
      "views", "VIDEO", "Video", "Episode", "Sign in", "Play next", "Start mix",
      "Like this song", "Save this for later", "Music videos", "Playlist", "Profile",
      "Podcast", "Single", "EP", NULL};
  if (!s || !s[0]) return 1;
  for (int i = 0; bad[i]; i++) {
    if (strcmp(s, bad[i]) == 0) return 1;
  }
  return 0;
}

static int clock_seconds(const char *s) {
  int a = 0, b = 0, c = 0;
  if (sscanf(s, "%d:%d:%d", &a, &b, &c) == 3) return a * 3600 + b * 60 + c;
  if (sscanf(s, "%d:%d", &a, &b) == 2 && b < 60) return a * 60 + b;
  return -1;
}

int net_init(char *err, int err_n) {
  g_resp = (char *)malloc(RESP_MAX);
  if (!g_resp) {
    snprintf(err, (size_t)err_n, "Out of memory");
    return -1;
  }
  if (sceNetInit() != 0) {
    snprintf(err, (size_t)err_n, "sceNetInit failed");
    return -1;
  }
  g_net = sceNetPoolCreate("ytmusic", 256 * 1024, 0);
  if (g_net < 0) {
    snprintf(err, (size_t)err_n, "sceNetPoolCreate %d", g_net);
    return -1;
  }
  g_ssl = sceSslInit(512 * 1024);
  if (g_ssl < 0) {
    snprintf(err, (size_t)err_n, "sceSslInit %d", g_ssl);
    return -1;
  }
  g_http = sceHttp2Init(g_net, g_ssl, 512 * 1024, 1);
  if (g_http < 0) {
    snprintf(err, (size_t)err_n, "sceHttp2Init %d", g_http);
    return -1;
  }
  g_tmpl = sceHttp2CreateTemplate(
      g_http,
      "com.google.android.apps.youtube.music/7.27.52 (Linux; U; Android 11) gzip",
      3, 1);
  if (g_tmpl < 0) {
    snprintf(err, (size_t)err_n, "sceHttp2CreateTemplate %d", g_tmpl);
    return -1;
  }
  sceHttp2SetAutoRedirect(g_tmpl, 1);
  err[0] = 0;
  return 0;
}

void net_shutdown(void) {
  if (g_tmpl >= 0) sceHttp2DeleteTemplate(g_tmpl);
  if (g_http >= 0) sceHttp2Term(g_http);
  if (g_ssl >= 0) sceSslTerm(g_ssl);
  if (g_net >= 0) sceNetPoolDestroy(g_net);
  cover_clear();
  free(g_resp);
  g_resp = NULL;
  g_tmpl = g_http = g_ssl = g_net = -1;
}

static char g_access[2048];
static char g_refresh[1024];
static char g_device_code[256];
static char g_visitor[128];
static time_t g_access_exp;
static int g_skip_visitor;
static int g_send_reqtime;
static int g_skip_authuser;
static const char *g_api_format;
static const char *g_accept;
static const char *g_cookie;
static const char *g_visitor_override;
static char g_music_visitor[128];

static int http_post_ex(const char *url, const char *body, const char *ua, const char *origin,
                         const char *referer, const char *client_name, const char *client_ver,
                         int authed, int soft, int *status, char *err, int err_n) {
  int req;
  int total = 0;
  int n;
  size_t body_n;
  if (!g_resp || g_tmpl < 0) {
    snprintf(err, (size_t)err_n, "Network is not up");
    return -1;
  }
  body_n = strlen(body);
  req = sceHttp2CreateRequestWithURL(g_tmpl, "POST", url, (unsigned long long)body_n);
  if (req < 0) {
    snprintf(err, (size_t)err_n, "HTTP create %d", req);
    return -1;
  }
  sceHttp2AddRequestHeader(req, "Content-Type", "application/json", SCE_HTTP_HEADER_OVERWRITE);
  sceHttp2AddRequestHeader(req, "Accept", g_accept && g_accept[0] ? g_accept : "application/json",
                           SCE_HTTP_HEADER_OVERWRITE);
  if (ua) sceHttp2AddRequestHeader(req, "User-Agent", ua, SCE_HTTP_HEADER_OVERWRITE);
  if (origin) sceHttp2AddRequestHeader(req, "Origin", origin, SCE_HTTP_HEADER_OVERWRITE);
  if (referer) sceHttp2AddRequestHeader(req, "Referer", referer, SCE_HTTP_HEADER_OVERWRITE);
  if (g_cookie && g_cookie[0])
    sceHttp2AddRequestHeader(req, "Cookie", g_cookie, SCE_HTTP_HEADER_OVERWRITE);
  if (client_name)
    sceHttp2AddRequestHeader(req, "X-YouTube-Client-Name", client_name, SCE_HTTP_HEADER_OVERWRITE);
  if (client_ver)
    sceHttp2AddRequestHeader(req, "X-YouTube-Client-Version", client_ver, SCE_HTTP_HEADER_OVERWRITE);
  if (g_api_format && g_api_format[0])
    sceHttp2AddRequestHeader(req, "X-Goog-Api-Format-Version", g_api_format,
                             SCE_HTTP_HEADER_OVERWRITE);
  if (g_visitor_override && g_visitor_override[0])
    sceHttp2AddRequestHeader(req, "X-Goog-Visitor-Id", g_visitor_override, SCE_HTTP_HEADER_OVERWRITE);
  else if (g_visitor[0] && !g_skip_visitor)
    sceHttp2AddRequestHeader(req, "X-Goog-Visitor-Id", g_visitor, SCE_HTTP_HEADER_OVERWRITE);
  if (g_send_reqtime) {
    char ts[32];
    snprintf(ts, sizeof ts, "%lld", (long long)time(NULL));
    sceHttp2AddRequestHeader(req, "X-Goog-Request-Time", ts, SCE_HTTP_HEADER_OVERWRITE);
  }
  if (authed && g_access[0]) {
    char auth[2100];
    snprintf(auth, sizeof auth, "Bearer %s", g_access);
    sceHttp2AddRequestHeader(req, "Authorization", auth, SCE_HTTP_HEADER_OVERWRITE);
    if (!g_skip_authuser)
      sceHttp2AddRequestHeader(req, "X-Goog-AuthUser", "0", SCE_HTTP_HEADER_OVERWRITE);
  }
  if (sceHttp2SendRequest(req, body, body_n) != 0) {
    sceHttp2DeleteRequest(req);
    snprintf(err, (size_t)err_n, "HTTP send failed");
    return -1;
  }
  *status = 0;
  sceHttp2GetStatusCode(req, status);
  while (total + 1 < RESP_MAX &&
         (n = sceHttp2ReadData(req, g_resp + total, (size_t)(RESP_MAX - total - 1))) > 0) {
    total += n;
  }
  g_resp[total] = 0;
  sceHttp2DeleteRequest(req);
  if (*status != 200 && !soft) {
    snprintf(err, (size_t)err_n, "YouTube HTTP %d", *status);
    return -1;
  }
  if (total < 2 && !soft) {
    snprintf(err, (size_t)err_n, "Empty response");
    return -1;
  }
  err[0] = 0;
  return total;
}

static int http_post(const char *url, const char *body, int *status, char *err, int err_n) {
  return http_post_ex(url, body, NULL, "https://music.youtube.com", "https://music.youtube.com/",
                      NULL, NULL, 0, 0, status, err, err_n);
}

static const char *find_bounded(const char *p, const char *end, const char *pat) {
  size_t n = strlen(pat);
  if (!p || !end || end < p || (size_t)(end - p) < n) return NULL;
  for (; p + n <= end; p++) {
    if (memcmp(p, pat, n) == 0) return p;
  }
  return NULL;
}

static const char *json_end(const char *p) {
  char open;
  int depth = 0;
  int in_str = 0;
  if (!p || (*p != '{' && *p != '[')) return p ? p + 1 : p;
  open = *p;
  (void)open;
  for (; *p; p++) {
    if (in_str) {
      if (*p == '\\' && p[1]) {
        p++;
        continue;
      }
      if (*p == '"') in_str = 0;
      continue;
    }
    if (*p == '"') {
      in_str = 1;
      continue;
    }
    if (*p == '{' || *p == '[') depth++;
    else if (*p == '}' || *p == ']') {
      depth--;
      if (depth == 0) return p + 1;
    }
  }
  return p;
}

static int is_sep(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  int marks = 0;
  if (!s || !s[0]) return 0;
  while (*p) {
    if (*p == ' ' || *p == '\t') {
      p++;
      continue;
    }
    if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA2 || p[2] == 0xA7)) {
      marks++;
      p += 3;
      continue;
    }
    if (*p == '|' || *p == 0xB7) {
      marks++;
      p++;
      continue;
    }
    return 0;
  }
  return marks > 0;
}

static int is_noise(const char *s) {
  if (!s || !s[0]) return 1;
  if (junk_label(s)) return 1;
  if (strstr(s, " view") || strstr(s, " play") || strstr(s, "audience") || strstr(s, "listener") ||
      strstr(s, " ago") || strstr(s, "monthly") || strstr(s, "subscriber"))
    return 1;
  if (strlen(s) > 3 && s[3] == ' ') {
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int i;
    for (i = 0; i < 36; i += 3) {
      if (strncmp(s, mon + i, 3) == 0) return 1;
    }
  }
  return 0;
}

static void store_thumb(Track *t, const char *url) {
  const char *eq;
  int n;
  if (t->id[0]) {
    snprintf(t->thumb, sizeof t->thumb, "https://i.ytimg.com/vi/%s/hq720.jpg", t->id);
    return;
  }
  if (!url || !url[0]) return;
  eq = strstr(url, "=w");
  if (!eq) eq = strstr(url, "=s");
  if (eq && (int)(eq - url) < (int)sizeof t->thumb - 20) {
    n = (int)(eq - url);
    memcpy(t->thumb, url, (size_t)n);
    snprintf(t->thumb + n, sizeof t->thumb - (size_t)n, "=w720-h720-l90-rj");
    return;
  }
  if ((int)strlen(url) < (int)sizeof t->thumb) snprintf(t->thumb, sizeof t->thumb, "%s", url);
}

static void finish_track(Track *t) {
  if (!t->title[0]) {
    if (t->id[0]) snprintf(t->title, sizeof t->title, "%s", t->id);
    else if (t->browse[0]) snprintf(t->title, sizeof t->title, "%s", "Playlist");
  }
  if (!t->artist[0])
    snprintf(t->artist, sizeof t->artist, "%s", t->browse[0] && !t->id[0] ? "Playlist" : "YouTube Music");
  store_thumb(t, t->thumb);
}

static void note_browse_ids(const char *s, const char *end, Track *t) {
  const char *p = s;
  while ((p = find_bounded(p, end, "\"browseId\":\"")) != NULL) {
    char id[YTM_BROWSE_LEN];
    copy_json_str(p + 12, id, (int)sizeof id);
    p += 12;
    if (strncmp(id, "MPREb_", 6) == 0 && !t->album_id[0])
      snprintf(t->album_id, sizeof t->album_id, "%s", id);
    else if (strncmp(id, "UC", 2) == 0 && strlen(id) > 8 && !t->artist_id[0])
      snprintf(t->artist_id, sizeof t->artist_id, "%s", id);
  }
}

static int take_vid(const char *p, const char *end, char *id) {
  if (!p || p + 12 > end || p[11] != '"') return 0;
  memcpy(id, p, 11);
  id[11] = 0;
  return id_ok(id);
}

static void note_part(Track *t, const char *s) {
  int sec;
  if (!s || !s[0] || is_sep(s)) return;
  sec = clock_seconds(s);
  if (sec >= 0) {
    if (t->seconds <= 0) t->seconds = sec;
    return;
  }
  if (is_noise(s)) return;
  if (t->title[0] && strcmp(s, t->title) == 0) return;
  if (t->artist[0] && strcmp(s, t->artist) == 0) return;
  if (!t->title[0]) snprintf(t->title, sizeof t->title, "%s", s);
  else if (!t->artist[0]) snprintf(t->artist, sizeof t->artist, "%s", s);
  else if (!t->album[0] && strcmp(s, t->artist) != 0) snprintf(t->album, sizeof t->album, "%s", s);
}

static void push_field(char *cur, Track *t) {
  if (cur[0]) note_part(t, cur);
  cur[0] = 0;
}

static void runs_into(const char *s, const char *end, Track *t, int as_title) {
  const char *p = s;
  char cur[YTM_TITLE_LEN];
  char piece[YTM_TITLE_LEN];
  if (as_title) {
    const char *tx = find_bounded(s, end, "\"text\":\"");
    if (!tx) tx = find_bounded(s, end, "\"simpleText\":\"");
    if (!tx) return;
    copy_json_str(tx + (tx[1] == 's' ? 14 : 8), piece, (int)sizeof piece);
    if (piece[0] && !t->title[0]) snprintf(t->title, sizeof t->title, "%s", piece);
    return;
  }
  cur[0] = 0;
  while (p && p < end) {
    const char *a = find_bounded(p, end, "\"text\":\"");
    const char *b = find_bounded(p, end, "\"simpleText\":\"");
    const char *hit;
    int simple;
    if (a && (!b || a <= b)) {
      hit = a;
      simple = 0;
    } else if (b) {
      hit = b;
      simple = 1;
    } else {
      break;
    }
    copy_json_str(hit + (simple ? 14 : 8), piece, (int)sizeof piece);
    p = hit + (simple ? 14 : 8);
    if (!piece[0]) continue;
    if (is_sep(piece)) {
      push_field(cur, t);
      continue;
    }
    if ((int)strlen(cur) + (int)strlen(piece) + 1 >= (int)sizeof cur) {
      push_field(cur, t);
      continue;
    }
    memcpy(cur + strlen(cur), piece, strlen(piece) + 1);
  }
  push_field(cur, t);
}

static void add_track(Track *out, int *count, int max, Track *t) {
  int i;
  if (!t->id[0] && !t->browse[0]) return;
  if (!t->title[0] && !t->artist[0]) return;
  finish_track(t);
  if (t->id[0]) {
    for (i = 0; i < *count; i++) {
      if (out[i].id[0] && strcmp(out[i].id, t->id) == 0) return;
    }
  } else if (t->browse[0]) {
    for (i = 0; i < *count; i++) {
      if (!out[i].id[0] && strcmp(out[i].browse, t->browse) == 0) return;
    }
  }
  if (*count >= max) return;
  out[*count] = *t;
  (*count)++;
}

static int walk_kind(const char *json, const char *marker, Track *out, int *count, int max,
                     void (*parse)(const char *, const char *, Track *)) {
  const char *p = json;
  while (*count < max && (p = strstr(p, marker)) != NULL) {
    const char *obj = strchr(p, '{');
    const char *end;
    Track t;
    if (!obj) break;
    end = json_end(obj);
    memset(&t, 0, sizeof t);
    parse(obj, end, &t);
    add_track(out, count, max, &t);
    p = end > p ? end : p + 1;
  }
  return *count;
}

static void parse_mrlir(const char *s, const char *end, Track *t) {
  const char *menu = find_bounded(s, end, "\"menu\":");
  const char *lim = menu ? menu : end;
  const char *pid = find_bounded(s, lim, "\"playlistItemData\":");
  const char *vid = NULL;
  const char *fc, *fe, *q;
  const char *fixed;
  if (pid) {
    const char *obj = strchr(pid, '{');
    const char *pend = obj ? json_end(obj) : lim;
    if (pend > lim) pend = lim;
    vid = obj ? find_bounded(obj, pend, "\"videoId\":\"") : NULL;
  }
  if (!vid) vid = find_bounded(s, lim, "\"videoId\":\"");
  if (!vid || !take_vid(vid + 11, lim, t->id)) {
    const char *pl = find_bounded(s, lim, "\"playlistId\":\"");
    char pid[64];
    t->id[0] = 0;
    pid[0] = 0;
    if (pl) copy_json_str(pl + 14, pid, (int)sizeof pid);
    /* Radio mixes (RD…) are not a stable shelf. Real playlists browse as VL + id. */
    if (pid[0] && strncmp(pid, "RD", 2) != 0 && (int)strlen(pid) + 3 < YTM_BROWSE_LEN) {
      if (strncmp(pid, "VL", 2) == 0)
        memcpy(t->browse, pid, strlen(pid) + 1);
      else {
        t->browse[0] = 'V';
        t->browse[1] = 'L';
        memcpy(t->browse + 2, pid, strlen(pid) + 1);
      }
    }
    if (!t->browse[0]) return;
  }
  fc = find_bounded(s, lim, "\"flexColumns\":");
  if (fc) {
    const char *arr = strchr(fc, '[');
    fe = arr ? json_end(arr) : lim;
    if (fe > lim) fe = lim;
    q = arr ? arr : fc;
    while (q && (q = find_bounded(q, fe, "\"musicResponsiveListItemFlexColumnRenderer\":"))) {
      const char *obj = strchr(q, '{');
      const char *col = obj ? json_end(obj) : fe;
      if (!obj || col > fe) break;
      if (!t->title[0]) runs_into(obj, col, t, 1);
      else runs_into(obj, col, t, 0);
      q = col;
    }
  }
  fixed = find_bounded(s, lim, "\"fixedColumns\":");
  if (fixed) {
    const char *arr = strchr(fixed, '[');
    const char *fend = arr ? json_end(arr) : lim;
    const char *tx;
    if (fend > lim) fend = lim;
    tx = arr ? find_bounded(arr, fend, "\"text\":\"") : NULL;
    if (tx) {
      char tmp[YTM_TITLE_LEN];
      copy_json_str(tx + 8, tmp, (int)sizeof tmp);
      if (clock_seconds(tmp) >= 0) t->seconds = clock_seconds(tmp);
    }
  }
  note_browse_ids(s, end, t);
}

static void parse_card(const char *s, const char *end, Track *t) {
  const char *title = find_bounded(s, end, "\"title\":");
  const char *sub = find_bounded(s, end, "\"subtitle\":");
  const char *vid = find_bounded(s, end, "\"videoId\":\"");
  const char *tend;
  if (!vid || !take_vid(vid + 11, end, t->id)) return;
  if (title) {
    const char *obj = strchr(title, '{');
    tend = obj ? json_end(obj) : (sub ? sub : end);
    if (obj) runs_into(obj, tend > end ? end : tend, t, 1);
  }
  if (sub && sub < end) {
    const char *obj = strchr(sub, '{');
    const char *send = obj ? json_end(obj) : end;
    if (send > end) send = end;
    if (obj) runs_into(obj, send, t, 0);
  }
  note_browse_ids(s, end, t);
}

static void parse_tile(const char *s, const char *end, Track *t) {
  const char *id = find_bounded(s, end, "\"contentId\":\"");
  const char *meta = find_bounded(s, end, "\"tileMetadataRenderer\":");
  const char *ov = find_bounded(s, end, "\"thumbnailOverlayTimeStatusRenderer\":");
  if (!id || !take_vid(id + 13, end, t->id)) return;
  if (meta) {
    const char *obj = strchr(meta, '{');
    const char *mend = obj ? json_end(obj) : end;
    if (mend > end) mend = end;
    if (obj) runs_into(obj, mend, t, 0);
  }
  if (ov) {
    const char *obj = strchr(ov, '{');
    const char *oend = obj ? json_end(obj) : end;
    const char *tx;
    if (oend > end) oend = end;
    tx = obj ? find_bounded(obj, oend, "\"simpleText\":\"") : NULL;
    if (tx) {
      char tmp[64];
      copy_json_str(tx + 13, tmp, (int)sizeof tmp);
      if (clock_seconds(tmp) >= 0) t->seconds = clock_seconds(tmp);
    }
  }
  note_browse_ids(s, end, t);
}

static int good_browse(const char *id) {
  size_t n;
  if (!id) return 0;
  n = strlen(id);
  if (n < 4 || n >= YTM_BROWSE_LEN) return 0;
  /* Playlists browse as VL…, albums as MPREb_. Podcasts (MPSP) and channels are not shelves. */
  if (strncmp(id, "VL", 2) == 0) return 1;
  if (strncmp(id, "MPREb_", 6) == 0) return 1;
  return 0;
}

static int page_is(const char *s, const char *end, const char *type) {
  const char *p = find_bounded(s, end, "\"pageType\":\"");
  size_t n;
  if (!p || !type) return 0;
  p += 12;
  n = strlen(type);
  if ((size_t)(end - p) < n + 1) return 0;
  return strncmp(p, type, n) == 0 && p[n] == '"';
}

/* Item click target only. Album and artist links inside the subtitle are nested
 * deeper, and treating those as the row dropped every quick pick. */
static const char *top_key(const char *s, const char *end, const char *key) {
  int depth = 0;
  int in_str = 0;
  size_t kn = strlen(key);
  const char *p;
  if (!s || *s != '{') return find_bounded(s, end, key);
  for (p = s; p < end; p++) {
    if (in_str) {
      if (*p == '\\' && p + 1 < end) {
        p++;
        continue;
      }
      if (*p == '"') in_str = 0;
      continue;
    }
    if (*p == '"') {
      if (depth == 1 && (size_t)(end - p) >= kn && memcmp(p, key, kn) == 0) return p;
      in_str = 1;
      continue;
    }
    if (*p == '{' || *p == '[') depth++;
    else if (*p == '}' || *p == ']') {
      depth--;
      if (depth <= 0) break;
    }
  }
  return NULL;
}

static int nav_page(const char *s, const char *end, const char *type) {
  const char *nav = top_key(s, end, "\"navigationEndpoint\"");
  const char *obj;
  const char *lim;
  if (!nav) return 0;
  obj = strchr(nav, '{');
  if (!obj || obj >= end) return 0;
  lim = json_end(obj);
  if (lim > end) lim = end;
  return page_is(obj, lim, type);
}

static void store_playlist(Track *t, const char *pid) {
  if (!pid || !pid[0] || strncmp(pid, "RD", 2) == 0) return;
  if ((int)strlen(pid) + 3 >= YTM_BROWSE_LEN) return;
  if (strncmp(pid, "VL", 2) == 0) {
    memcpy(t->browse, pid, strlen(pid) + 1);
    return;
  }
  t->browse[0] = 'V';
  t->browse[1] = 'L';
  memcpy(t->browse + 2, pid, strlen(pid) + 1);
}

static void parse_two(const char *s, const char *end, Track *t) {
  const char *menu = find_bounded(s, end, "\"menu\":");
  const char *lim = menu ? menu : end;
  const char *title = find_bounded(s, lim, "\"title\":");
  const char *sub = find_bounded(s, lim, "\"subtitle\":");
  const char *vid;
  const char *p = s;
  char url[300];
  int shelf;
  if (nav_page(s, end, "MUSIC_PAGE_TYPE_PODCAST") || nav_page(s, end, "MUSIC_PAGE_TYPE_ARTIST") ||
      nav_page(s, end, "MUSIC_PAGE_TYPE_USER_CHANNEL"))
    return;
  shelf = nav_page(s, end, "MUSIC_PAGE_TYPE_ALBUM") || nav_page(s, end, "MUSIC_PAGE_TYPE_PLAYLIST");
  while ((p = find_bounded(p, lim, "\"browseId\":\"")) != NULL) {
    char id[YTM_BROWSE_LEN];
    copy_json_str(p + 12, id, (int)sizeof id);
    p += 12;
    if (good_browse(id)) {
      snprintf(t->browse, sizeof t->browse, "%s", id);
      break;
    }
  }
  if (!t->browse[0]) {
    const char *pl = find_bounded(s, lim, "\"playlistId\":\"");
    char pid[64];
    pid[0] = 0;
    if (pl) copy_json_str(pl + 14, pid, (int)sizeof pid);
    store_playlist(t, pid);
  }
  vid = find_bounded(s, lim, "\"videoId\":\"");
  if (vid && !shelf) take_vid(vid + 11, lim, t->id);
  if (!t->browse[0] && !t->id[0]) return;
  /* A song keeps its album id for the menu. The browse field is the shelf. */
  if (t->id[0] && strncmp(t->browse, "MPREb_", 6) == 0) t->browse[0] = 0;
  if (title) {
    const char *obj = strchr(title, '{');
    const char *tend = obj ? json_end(obj) : lim;
    if (tend > lim) tend = lim;
    if (obj) runs_into(obj, tend, t, 1);
  }
  if (sub) {
    const char *obj = strchr(sub, '{');
    const char *send = obj ? json_end(obj) : lim;
    if (send > lim) send = lim;
    if (obj) runs_into(obj, send, t, 0);
  }
  note_browse_ids(s, end, t);
  url[0] = 0;
  p = find_bounded(s, lim, "\"url\":\"");
  if (p) copy_json_str(p + 7, url, (int)sizeof url);
  store_thumb(t, url);
}

static int collect_api(const char *json, Track *out, int max) {
  const char *p = json;
  int count = 0;
  while (count < max && (p = strstr(p, "\"contentDetails\"")) != NULL) {
    const char *id = strstr(p, "\"videoId\":\"");
    const char *base = (p - json > 1600) ? p - 1600 : json;
    const char *title = NULL;
    const char *artist = NULL;
    const char *owner = NULL;
    const char *q;
    Track t;
    if (!id || id > p + 400) {
      p += 16;
      continue;
    }
    memset(&t, 0, sizeof t);
    if (!take_vid(id + 11, id + 40, t.id)) {
      p = id + 11;
      continue;
    }
    q = base;
    while ((q = strstr(q, "\"title\":\"")) != NULL && q < p) {
      title = q + 9;
      q += 9;
    }
    q = base;
    while ((q = strstr(q, "\"videoOwnerChannelTitle\":\"")) != NULL && q < p) {
      owner = q + 26;
      q += 26;
    }
    q = base;
    while ((q = strstr(q, "\"channelTitle\":\"")) != NULL && q < p) {
      artist = q + 16;
      q += 16;
    }
    if (title) copy_json_str(title, t.title, (int)sizeof t.title);
    if (owner) copy_json_str(owner, t.artist, (int)sizeof t.artist);
    else if (artist) copy_json_str(artist, t.artist, (int)sizeof t.artist);
    add_track(out, &count, max, &t);
    p = id + 20;
  }
  return count;
}

static int collect_tracks(const char *json, Track *out, int max) {
  int count = 0;
  if (!json || max < 1) return 0;
  walk_kind(json, "\"musicResponsiveListItemRenderer\":", out, &count, max, parse_mrlir);
  walk_kind(json, "\"musicCardShelfRenderer\":", out, &count, max, parse_card);
  if (count == 0) walk_kind(json, "\"tileRenderer\":", out, &count, max, parse_tile);
  if (count == 0) walk_kind(json, "\"musicTwoRowItemRenderer\":", out, &count, max, parse_two);
  if (count == 0) count = collect_api(json, out, max);
  return count;
}

int ytm_search(const char *query, Track *out, int max, char *err, int err_n) {
  char esc[160];
  char body[512];
  char url[256];
  int status = 0;
  int n;
  json_escape(query, esc, (int)sizeof esc);
  snprintf(url, sizeof url,
           "https://music.youtube.com/youtubei/v1/search?key=%s&prettyPrint=false",
           INNERTUBE_KEY);
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
           "\"clientVersion\":\"1.20251001.01.00\",\"hl\":\"en\",\"gl\":\"US\"}},"
           "\"query\":\"%s\"}",
           esc);
  n = http_post(url, body, &status, err, err_n);
  if (n < 0) return -1;
  n = collect_tracks(g_resp, out, max);
  if (n == 0) snprintf(err, (size_t)err_n, "No songs for that search");
  else err[0] = 0;
  return n;
}

static const char *url_near(const char *mime, const char *json) {
  const char *from = json;
  const char *q;
  const char *found = NULL;
  /* This player response puts "url" just before "mimeType" in the same object. */
  if (mime - json > 1800) from = mime - 1800;
  q = from;
  while ((q = strstr(q, "\"url\":\"")) != NULL && q < mime) {
    found = q + 7;
    q += 7;
  }
  if (found) return found;
  q = strstr(mime, "\"url\":\"");
  if (q && q < mime + 900) return q + 7;
  return NULL;
}

static int nearby_int(const char *p, const char *key, int window) {
  char pat[40];
  const char *q;
  const char *end = p + window;
  snprintf(pat, sizeof pat, "\"%s\":", key);
  q = strstr(p, pat);
  if (!q || q >= end) return 0;
  q += strlen(pat);
  while (*q == ' ') q++;
  return atoi(q);
}

static int pick_audio_url(const char *json, char *url, int url_n) {
  const char *p = json;
  int best = -1;
  while ((p = strstr(p, "\"mimeType\":\"audio/mp4")) != NULL) {
    int rate = nearby_int(p, "bitrate", 240);
    const char *raw = url_near(p, json);
    if (raw && rate >= best) {
      char decoded[4096];
      copy_json_str(raw, decoded, (int)sizeof decoded);
      if (strncmp(decoded, "https://", 8) == 0 && strstr(decoded, "googlevideo.com")) {
        snprintf(url, (size_t)url_n, "%s", decoded);
        best = rate > 0 ? rate : 0;
      }
    }
    p += 22;
  }
  return best >= 0;
}

static char g_stream_ua[180];
static char g_stream_ref[96];

static int ensure_access(char *err, int err_n);

/* Anonymous player calls are rejected as a bot until this id is sent back. */
static int ensure_visitor(void) {
  char err[80];
  char body[192];
  int status = 0;
  int n;
  if (g_visitor[0]) return 0;
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"WEB\","
           "\"clientVersion\":\"2.20251009.00.00\",\"hl\":\"en\",\"gl\":\"US\"}}}");
  n = http_post_ex("https://www.youtube.com/youtubei/v1/visitor_id?prettyPrint=false", body,
                   "Mozilla/5.0", "https://www.youtube.com", "https://www.youtube.com/", "1",
                   "2.20251009.00.00", 0, 1, &status, err, (int)sizeof err);
  if (n < 2 || status != 200 ||
      !json_string(g_resp, "visitorData", g_visitor, (int)sizeof g_visitor) ||
      strchr(g_visitor, '"') || strchr(g_visitor, '\\')) {
    g_visitor[0] = 0;
    return -1;
  }
  return 0;
}

static void remember_stream(const char *ua, const char *ref) {
  snprintf(g_stream_ua, sizeof g_stream_ua, "%s", ua ? ua : "");
  snprintf(g_stream_ref, sizeof g_stream_ref, "%s", ref ? ref : "");
}

const char *ytm_stream_ua(void) {
  return g_stream_ua[0] ? g_stream_ua
                        : "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
                          "(KHTML, like Gecko) Version/26.0 Safari/605.1.15";
}

const char *ytm_stream_referer(void) {
  return g_stream_ref[0] ? g_stream_ref : "https://www.youtube.com/";
}

static void player_fail(char *err, int err_n) {
  char reason[160];
  if (strstr(g_resp, "signatureCipher") || strstr(g_resp, "\"signature\":\""))
    snprintf(err, (size_t)err_n, "YouTube signed this stream; no direct audio URL");
  else if (strstr(g_resp, "LOGIN_REQUIRED"))
    snprintf(err, (size_t)err_n, "YouTube asked this console to sign in");
  else if (json_string(g_resp, "reason", reason, (int)sizeof reason))
    snprintf(err, (size_t)err_n, "%s", reason);
  else
    snprintf(err, (size_t)err_n, "No audio URL in the player response");
}

static int player_call(const char *video_id, const char *host, const char *client,
                       const char *ver, const char *ua, const char *referer, const char *cname,
                       const char *extra, int authed, char *url, int url_n, int *duration,
                       char *err, int err_n) {
  char body[1536];
  char endpoint[240];
  char vis[160];
  int status = 0;
  int len = 0;
  int n;
  vis[0] = 0;
  if (g_visitor[0]) snprintf(vis, sizeof vis, ",\"visitorData\":\"%s\"", g_visitor);
  snprintf(endpoint, sizeof endpoint, "https://%s/youtubei/v1/player?prettyPrint=false", host);
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"%s\",\"clientVersion\":\"%s\","
           "\"hl\":\"en\",\"gl\":\"US\",\"userAgent\":\"%s\"%s%s}},\"videoId\":\"%s\","
           "\"contentCheckOk\":true,\"racyCheckOk\":true}",
           client, ver, ua, vis, extra ? extra : "", video_id);
  n = http_post_ex(endpoint, body, ua, "https://www.youtube.com", referer, cname, ver, authed, 1,
                   &status, err, err_n);
  if (n < 2) {
    if (err[0] == 0) snprintf(err, (size_t)err_n, "Player request failed");
    return -1;
  }
  json_int(g_resp, "lengthSeconds", &len);
  if (len <= 0) {
    int ms = 0;
    if (json_int(g_resp, "approxDurationMs", &ms) && ms > 0) len = ms / 1000;
  }
  if (pick_audio_url(g_resp, url, url_n)) {
    *duration = len;
    remember_stream(ua, referer);
    err[0] = 0;
    return 0;
  }
  player_fail(err, err_n);
  return -1;
}

int ytm_audio_url(const char *video_id, char *url, int url_n, int *duration, char *err,
                  int err_n) {
  static const char *vision_ua =
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/26.0 Safari/605.1.15";
  static const char *vision_extra =
      ",\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\","
      "\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\"";
  char gate[192];
  char kept[192];
  int signed_in = 0;
  int attempt;
  *duration = 0;
  url[0] = 0;
  gate[0] = 0;
  kept[0] = 0;
  /* OAuth is only honored on the TV client. Public songs still need a visitor
   * id or YouTube answers LOGIN_REQUIRED ("sign in to confirm you're not a bot"). */
  if (g_refresh[0] && ensure_access(gate, (int)sizeof gate) == 0) signed_in = 1;
  ensure_visitor();
  for (attempt = 0; attempt < 2; attempt++) {
    if (player_call(video_id, "www.youtube.com", "VISIONOS", "1.02", vision_ua,
                    "https://www.youtube.com/", "101", vision_extra, 0, url, url_n, duration, err,
                    err_n) == 0)
      return 0;
    if (!strstr(err, "sign in")) break;
    g_visitor[0] = 0;
    if (ensure_visitor() != 0) break;
  }
  snprintf(kept, sizeof kept, "%s", err);
  if (signed_in &&
      player_call(video_id, "www.youtube.com", "TVHTML5", "7.20261007.13.00",
                  "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold "
                  "(unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)",
                  "https://www.youtube.com/tv", "7", "", 1, url, url_n, duration, err,
                  err_n) == 0)
    return 0;
  if (kept[0]) snprintf(err, (size_t)err_n, "%s", kept);
  else if (!signed_in && gate[0]) snprintf(err, (size_t)err_n, "%s", gate);
  return -1;
}

/* Identity published by https://www.youtube.com/tv for its device-code sign-in.
 * OAuth on Innertube only works as this TV client, not as the music web client. */
static const char *TV_CLIENT_ID =
    "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com";
static const char *TV_CLIENT_SECRET = "SboVhoG9s0rNafixCSGGKXAT";
static const char *TV_VER = "7.20261007.13.00";
static const char *TV_UA = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version";

static void take_tokens(void) {
  char access[2048];
  char refresh[1024];
  int exp = 3600;
  access[0] = 0;
  refresh[0] = 0;
  json_string(g_resp, "access_token", access, (int)sizeof access);
  json_string(g_resp, "refresh_token", refresh, (int)sizeof refresh);
  json_int(g_resp, "expires_in", &exp);
  if (access[0]) {
    snprintf(g_access, sizeof g_access, "%s", access);
    if (exp < 120) exp = 120;
    g_access_exp = time(NULL) + exp - 60;
  }
  if (refresh[0]) snprintf(g_refresh, sizeof g_refresh, "%s", refresh);
}

static int token_error(char *err, int err_n) {
  char code[64];
  if (!json_string(g_resp, "error", code, (int)sizeof code)) {
    snprintf(err, (size_t)err_n, "Sign-in failed");
    return -1;
  }
  if (strcmp(code, "authorization_pending") == 0 || strcmp(code, "slow_down") == 0) return 1;
  if (strcmp(code, "access_denied") == 0)
    snprintf(err, (size_t)err_n, "Sign-in was declined");
  else if (strcmp(code, "expired_token") == 0)
    snprintf(err, (size_t)err_n, "That code expired. Press X for a new one");
  else if (strcmp(code, "invalid_grant") == 0)
    snprintf(err, (size_t)err_n, "Sign-in expired. Press X to start again");
  else
    snprintf(err, (size_t)err_n, "Sign-in failed (%s)", code);
  return -1;
}

int ytm_signed_in(void) { return g_access[0] != 0 && time(NULL) < g_access_exp; }

const char *ytm_refresh_token(void) { return g_refresh; }

void ytm_set_refresh_token(const char *token) {
  snprintf(g_refresh, sizeof g_refresh, "%s", token ? token : "");
}

void ytm_auth_signout(void) {
  g_access[0] = 0;
  g_refresh[0] = 0;
  g_device_code[0] = 0;
  g_access_exp = 0;
}

int ytm_auth_begin(char *user_code, int code_n, char *verify_url, int url_n, int *interval,
                   char *err, int err_n) {
  char body[640];
  char dev[40];
  unsigned seed;
  int status = 0;
  int n;
  int exp = 1800;
  seed = (unsigned)time(NULL);
  snprintf(dev, sizeof dev, "%08x-%04x-%04x-%04x-%012x", seed, (seed >> 4) & 0xffff,
           (seed >> 8) & 0xffff, (seed * 3) & 0xffff, seed * 17u);
  snprintf(body, sizeof body,
           "{\"client_id\":\"%s\",\"scope\":\"http://gdata.youtube.com "
           "https://www.googleapis.com/auth/youtube-paid-content\","
           "\"device_id\":\"%s\",\"device_model\":\"ytlr::\"}",
           TV_CLIENT_ID, dev);
  n = http_post_ex("https://www.youtube.com/o/oauth2/device/code", body, TV_UA,
                   "https://www.youtube.com", "https://www.youtube.com/tv", NULL, NULL, 0, 1,
                   &status, err, err_n);
  if (n < 2 || status != 200) {
    if (err[0] == 0) snprintf(err, (size_t)err_n, "Could not start sign-in");
    if (status && status != 200) token_error(err, err_n);
    return -1;
  }
  if (!json_string(g_resp, "device_code", g_device_code, (int)sizeof g_device_code) ||
      !json_string(g_resp, "user_code", user_code, code_n)) {
    snprintf(err, (size_t)err_n, "Sign-in response had no code");
    return -1;
  }
  if (!json_string(g_resp, "verification_url", verify_url, url_n))
    snprintf(verify_url, (size_t)url_n, "%s", "https://www.google.com/device");
  *interval = 5;
  json_int(g_resp, "interval", interval);
  if (*interval < 5) *interval = 5;
  json_int(g_resp, "expires_in", &exp);
  (void)exp;
  err[0] = 0;
  return 0;
}

static int post_token(const char *body, char *err, int err_n) {
  int status = 0;
  int n = http_post_ex("https://www.youtube.com/o/oauth2/token", body, TV_UA,
                       "https://www.youtube.com", "https://www.youtube.com/tv", NULL, NULL, 0, 1,
                       &status, err, err_n);
  if (n < 2) {
    if (err[0] == 0) snprintf(err, (size_t)err_n, "Sign-in request failed");
    return -1;
  }
  if (status == 200 && strstr(g_resp, "access_token")) {
    take_tokens();
    if (!g_access[0]) {
      snprintf(err, (size_t)err_n, "Sign-in response had no token");
      return -1;
    }
    err[0] = 0;
    return 0;
  }
  return token_error(err, err_n);
}

int ytm_auth_poll(char *err, int err_n) {
  char body[1600];
  if (!g_device_code[0]) {
    snprintf(err, (size_t)err_n, "Sign-in has not started");
    return -1;
  }
  snprintf(body, sizeof body,
           "{\"client_id\":\"%s\",\"client_secret\":\"%s\",\"code\":\"%s\","
           "\"grant_type\":\"http://oauth.net/grant_type/device/1.0\"}",
           TV_CLIENT_ID, TV_CLIENT_SECRET, g_device_code);
  return post_token(body, err, err_n);
}

int ytm_auth_refresh(char *err, int err_n) {
  char body[1600];
  int rc;
  if (!g_refresh[0]) {
    snprintf(err, (size_t)err_n, "Not signed in");
    return -1;
  }
  snprintf(body, sizeof body,
           "{\"client_id\":\"%s\",\"client_secret\":\"%s\",\"refresh_token\":\"%s\","
           "\"grant_type\":\"refresh_token\"}",
           TV_CLIENT_ID, TV_CLIENT_SECRET, g_refresh);
  rc = post_token(body, err, err_n);
  if (rc == 0) g_device_code[0] = 0;
  return rc;
}

static int ensure_access(char *err, int err_n) {
  if (g_access[0] && time(NULL) < g_access_exp) return 0;
  return ytm_auth_refresh(err, err_n);
}

static int remix_browse_gl(const char *browse_id, const char *gl, int authed, Track *out, int max,
                           char *err, int err_n) {
  char body[768];
  char url[256];
  int status = 0;
  int n;
  const char *cc = (gl && gl[0] && gl[1]) ? gl : "US";
  snprintf(url, sizeof url,
           "https://music.youtube.com/youtubei/v1/browse?key=%s&prettyPrint=false", INNERTUBE_KEY);
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
           "\"clientVersion\":\"1.20251001.01.00\",\"hl\":\"en\",\"gl\":\"%s\"}},"
           "\"browseId\":\"%s\"}",
           cc, browse_id);
  if (http_post_ex(url, body, NULL, "https://music.youtube.com", "https://music.youtube.com/",
                   NULL, NULL, authed, 0, &status, err, err_n) < 0)
    return -1;
  n = collect_tracks(g_resp, out, max);
  if (n == 0) snprintf(err, (size_t)err_n, "Nothing in that shelf");
  return n;
}

static int remix_browse(const char *browse_id, int authed, Track *out, int max, char *err,
                        int err_n) {
  return remix_browse_gl(browse_id, "US", authed, out, max, err, err_n);
}

int ytm_browse(const char *browse_id, Track *out, int max, char *err, int err_n) {
  return remix_browse(browse_id, 0, out, max, err, err_n);
}

/* Bearer auth belongs on the TV client. WEB_REMIX rejects it with HTTP 400. */
static int tv_browse(const char *browse_id, Track *out, int max, char *err, int err_n) {
  char body[512];
  char url[192];
  int status = 0;
  int n;
  snprintf(url, sizeof url, "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false");
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\","
           "\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"US\"}},"
           "\"browseId\":\"%s\"}",
           TV_VER, browse_id);
  if (http_post_ex(url, body, TV_UA, "https://www.youtube.com", "https://www.youtube.com/tv", "7",
                   TV_VER, 1, 1, &status, err, err_n) < 0 || status != 200)
    return -1;
  n = collect_tracks(g_resp, out, max);
  if (n == 0) snprintf(err, (size_t)err_n, "No songs in that library");
  else err[0] = 0;
  return n;
}

int ytm_liked(Track *out, int max, char *err, int err_n) {
  int n = 0;
  if (ensure_access(err, err_n) != 0) return -1;
  /* WEB_REMIX plus a bearer token is a 400. The TV client accepts this token. */
  n = tv_browse("VLLM", out, max, err, err_n);
  if (n > 0) return n;
  n = tv_browse("FEmusic_liked_playlists", out, max, err, err_n);
  if (n > 0) return n;
  n = tv_browse("FEmusic_library_landing", out, max, err, err_n);
  if (n == 0 && !err[0]) snprintf(err, (size_t)err_n, "No songs in that library");
  return n;
}

static int keep_ids(Track *a, int n) {
  int i, w = 0;
  for (i = 0; i < n; i++) {
    if (a[i].id[0]) a[w++] = a[i];
  }
  return w;
}

static int keep_mixes(Track *a, int n) {
  int i, w = 0;
  for (i = 0; i < n; i++) {
    if (!a[i].id[0] && a[i].browse[0]) a[w++] = a[i];
  }
  return w;
}

/* Home and charts browse ids match ytmusicapi get_home / get_charts. See NOTICE. */
static char g_song_heading[40];
static char g_mix_heading[40];
static char g_place[8];
static int lookup_region(char *cc, int n);

const char *ytm_home_song_heading(void) {
  return g_song_heading[0] ? g_song_heading : "Quick play";
}
const char *ytm_home_mix_heading(void) { return g_mix_heading[0] ? g_mix_heading : "Playlists"; }
const char *ytm_home_place(void) {
  if (strcmp(g_place, "ZZ") == 0) return "Global";
  return g_place;
}

static int chart_rank(const Track *t) {
  if (!t || !t->title[0] || !t->browse[0]) return -1;
  if (strstr(t->title, "Podcast")) return -1;
  if (strstr(t->title, "Top 100 Music Videos")) return 50;
  if (strstr(t->title, "Daily Top")) return 40;
  if (strstr(t->title, "Trending")) return 20;
  if (strstr(t->title, "Live Performances")) return 10;
  return 1;
}

static Track g_raw[80];

static void split_home(const char *json, Track *songs, int song_max, int *nsongs, Track *mixes,
                       int mix_max, int *nmixes) {
  int n = 0;
  int i;
  /* One page mixes songs and albums. Classify after the walk so albums cannot
   * fill the song cap before the quick picks are seen. */
  walk_kind(json, "\"musicResponsiveListItemRenderer\":", g_raw, &n, 80, parse_mrlir);
  walk_kind(json, "\"musicCardShelfRenderer\":", g_raw, &n, 80, parse_card);
  walk_kind(json, "\"tileRenderer\":", g_raw, &n, 80, parse_tile);
  walk_kind(json, "\"musicTwoRowItemRenderer\":", g_raw, &n, 80, parse_two);
  for (i = 0; i < n; i++) {
    if (g_raw[i].id[0]) add_track(songs, nsongs, song_max, &g_raw[i]);
    else if (g_raw[i].browse[0]) add_track(mixes, nmixes, mix_max, &g_raw[i]);
  }
}

static void web_client_version(char *dst, int n) {
  time_t now = time(NULL);
  struct tm *tm = gmtime(&now);
  if (!tm) {
    snprintf(dst, (size_t)n, "1.20261009.01.00");
    return;
  }
  snprintf(dst, (size_t)n, "1.%04d%02d%02d.01.00", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

static int take_continuation(const char *json, char *out, int n) {
  const char *p;
  const char *c;
  if (!json || !out || n < 16) return 0;
  out[0] = 0;
  p = strstr(json, "\"nextContinuationData\"");
  if (!p) p = strstr(json, "\"continuationEndpoint\"");
  if (!p) return 0;
  c = strstr(p, "\"continuation\":\"");
  if (!c || c > p + 900) return 0;
  copy_json_str(c + 16, out, n);
  if ((int)strlen(out) < 12 || strchr(out, '"') || strchr(out, '\\')) {
    out[0] = 0;
    return 0;
  }
  return 1;
}

static void name_mixes(const Track *mixes, int n) {
  int albums = 0;
  int lists = 0;
  int i;
  for (i = 0; i < n; i++) {
    if (strncmp(mixes[i].browse, "MPREb_", 6) == 0) albums++;
    else lists++;
  }
  if (albums && lists)
    snprintf(g_mix_heading, sizeof g_mix_heading, "Albums & playlists");
  else if (albums)
    snprintf(g_mix_heading, sizeof g_mix_heading, "Albums");
  else
    snprintf(g_mix_heading, sizeof g_mix_heading, "Playlists");
}

/* Signed-in home is ytmusicapi YTMusic.get_home on an OAuth session.
 * URL is /youtubei/v1/browse?alt=json with no API key. Body is browseId
 * FEmusic_home plus the WEB_REMIX context. Headers are initialize_headers()
 * plus Authorization and X-Goog-Request-Time. The next page is the same body
 * with &ctoken=&continuation= on the query, not a different client. */
static int ensure_music_visitor(void);

static int post_account_home(const char *ctoken, int *status, char *err, int err_n) {
  char ver[32];
  char body[512];
  char url[4096];
  int skip;
  int rt;
  int skip_user;
  const char *accept;
  const char *cookie;
  const char *vis;
  int n;
  size_t ulen;
  web_client_version(ver, (int)sizeof ver);
  ensure_music_visitor();
  snprintf(url, sizeof url, "https://music.youtube.com/youtubei/v1/browse?alt=json");
  if (ctoken && ctoken[0] && !strchr(ctoken, '&') && !strchr(ctoken, ' ')) {
    ulen = strlen(url);
    if (strlen(ctoken) < 1600 && ulen + strlen(ctoken) * 2 + 32 < sizeof url)
      snprintf(url + ulen, sizeof url - ulen, "&ctoken=%s&continuation=%s", ctoken, ctoken);
  }
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
           "\"clientVersion\":\"%s\",\"hl\":\"en\"},\"user\":{}},"
           "\"browseId\":\"FEmusic_home\"}",
           ver);
  skip = g_skip_visitor;
  rt = g_send_reqtime;
  skip_user = g_skip_authuser;
  accept = g_accept;
  cookie = g_cookie;
  vis = g_visitor_override;
  g_skip_visitor = 1;
  g_send_reqtime = 1;
  g_skip_authuser = 1;
  g_accept = "*/*";
  g_cookie = "SOCS=CAI";
  g_visitor_override = g_music_visitor[0] ? g_music_visitor : NULL;
  n = http_post_ex(url, body,
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:88.0) Gecko/20100101 Firefox/88.0",
                   "https://music.youtube.com", NULL, NULL, NULL, 1, 1, status, err, err_n);
  g_skip_visitor = skip;
  g_send_reqtime = rt;
  g_skip_authuser = skip_user;
  g_accept = accept;
  g_cookie = cookie;
  g_visitor_override = vis;
  return n;
}

int ytm_home(Track *songs, int song_max, int *nsongs, Track *mixes, int mix_max, int *nmixes,
             char *err, int err_n) {
  char body[768];
  char url[256];
  char region[4];
  char ver[32];
  char saved[160];
  Track charts[12];
  int status = 0;
  int n;
  int ncharts = 0;
  int authed = 0;
  int best = -1;
  int bi = 0;
  if (nsongs) *nsongs = 0;
  if (nmixes) *nmixes = 0;
  g_song_heading[0] = 0;
  g_mix_heading[0] = 0;
  saved[0] = 0;
  if (!songs || !mixes || !nsongs || !nmixes || song_max < 1 || mix_max < 1) return -1;
  if (lookup_region(region, (int)sizeof region) != 0) snprintf(region, sizeof region, "ZZ");
  snprintf(g_place, sizeof g_place, "%s", region);
  if (g_refresh[0] && ensure_access(err, err_n) == 0) authed = 1;
  if (authed) {
    char cont[1800];
    int pass;
    cont[0] = 0;
    for (pass = 0; pass < 2; pass++) {
      n = post_account_home(pass ? cont : NULL, &status, err, err_n);
      if (n > 0 && status == 200 && g_resp && g_resp[0]) {
        split_home(g_resp, songs, song_max, nsongs, mixes, mix_max, nmixes);
        if (pass == 0 && (*nsongs < song_max || *nmixes < mix_max) &&
            take_continuation(g_resp, cont, (int)sizeof cont))
          continue;
      } else if (status > 0 && status != 200) {
        snprintf(saved, sizeof saved, "Home HTTP %d", status);
      } else if (n < 0 && err[0]) {
        snprintf(saved, sizeof saved, "%s", err);
      }
      break;
    }
    if (*nsongs > 0) snprintf(g_song_heading, sizeof g_song_heading, "Quick play");
    if (*nmixes > 0) name_mixes(mixes, *nmixes);
    if (*nsongs + *nmixes == 0) {
      if (saved[0]) snprintf(err, (size_t)err_n, "%s", saved);
      else snprintf(err, (size_t)err_n, "No recommendations for this account");
      return -1;
    }
    err[0] = 0;
    return 0;
  }
  /* Signed out: public home, then country charts. Same browse ids as get_home / get_charts. */
  web_client_version(ver, (int)sizeof ver);
  snprintf(url, sizeof url,
           "https://music.youtube.com/youtubei/v1/browse?key=%s&prettyPrint=false", INNERTUBE_KEY);
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
           "\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"%s\"}},"
           "\"browseId\":\"FEmusic_home\"}",
           ver, strcmp(region, "ZZ") == 0 ? "US" : region);
  if (http_post_ex(url, body, NULL, "https://music.youtube.com", "https://music.youtube.com/", NULL,
                   NULL, 0, 1, &status, err, err_n) > 0 &&
      status == 200 && g_resp && g_resp[0])
    split_home(g_resp, songs, song_max, nsongs, mixes, mix_max, nmixes);
  if (*nsongs > 0) snprintf(g_song_heading, sizeof g_song_heading, "Quick play");
  if (*nmixes > 0) name_mixes(mixes, *nmixes);
  if (*nsongs == 0 || *nmixes == 0) {
    snprintf(url, sizeof url,
             "https://music.youtube.com/youtubei/v1/browse?key=%s&prettyPrint=false", INNERTUBE_KEY);
    snprintf(body, sizeof body,
             "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\","
             "\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"%s\"}},"
             "\"browseId\":\"FEmusic_charts\",\"formData\":{\"selectedValues\":[\"%s\"]}}",
             ver, strcmp(region, "ZZ") == 0 ? "US" : region, region);
    if (http_post_ex(url, body, NULL, "https://music.youtube.com", "https://music.youtube.com/",
                     NULL, NULL, 0, 1, &status, err, err_n) > 0 &&
        status == 200 && g_resp && g_resp[0]) {
      n = 0;
      walk_kind(g_resp, "\"musicTwoRowItemRenderer\":", charts, &n, 12, parse_two);
      ncharts = keep_mixes(charts, n);
    }
  }
  if (*nmixes == 0 && ncharts > 0) {
    if (ncharts > mix_max) ncharts = mix_max;
    memcpy(mixes, charts, (size_t)ncharts * sizeof(Track));
    *nmixes = ncharts;
    snprintf(g_mix_heading, sizeof g_mix_heading, "Playlists");
  }
  if (*nsongs == 0 && ncharts > 0) {
    for (n = 0; n < ncharts; n++) {
      int rank = chart_rank(&charts[n]);
      if (rank > best) {
        best = rank;
        bi = n;
      }
    }
    if (best > 0) {
      n = remix_browse_gl(charts[bi].browse, region, 0, songs, song_max, err, err_n);
      if (n > 0) {
        *nsongs = keep_ids(songs, n);
        snprintf(g_song_heading, sizeof g_song_heading, "Top songs");
      }
    }
  }
  if (*nsongs + *nmixes == 0) {
    if (!err[0]) snprintf(err, (size_t)err_n, "No recommendations");
    return -1;
  }
  err[0] = 0;
  return 0;
}

#define PRE_SLOTS 4
typedef struct {
  char id[YTM_ID_LEN];
  char url[4096];
  int duration;
  int ready;
} PreSlot;
static PreSlot g_pre[PRE_SLOTS];
static int g_pre_next;

static PreSlot *pre_find(const char *id) {
  int i;
  if (!id || !id[0]) return NULL;
  for (i = 0; i < PRE_SLOTS; i++) {
    if (g_pre[i].id[0] && strcmp(g_pre[i].id, id) == 0) return &g_pre[i];
  }
  return NULL;
}

int ytm_prefetch_audio(const char *video_id) {
  PreSlot *slot;
  char err[192];
  char url[4096];
  int dur = 0;
  if (!video_id || !video_id[0]) return -1;
  slot = pre_find(video_id);
  if (slot && slot->ready) return 0;
  if (!slot) {
    slot = &g_pre[g_pre_next++ % PRE_SLOTS];
    slot->id[0] = 0;
    slot->ready = 0;
  }
  if (ytm_audio_url(video_id, url, (int)sizeof url, &dur, err, (int)sizeof err) != 0) return -1;
  snprintf(slot->id, sizeof slot->id, "%s", video_id);
  snprintf(slot->url, sizeof slot->url, "%s", url);
  slot->duration = dur;
  slot->ready = 1;
  return 0;
}

const char *ytm_prefetch_url(const char *video_id, int *duration) {
  PreSlot *slot = pre_find(video_id);
  if (!slot || !slot->ready) return NULL;
  if (duration && slot->duration > 0) *duration = slot->duration;
  return slot->url;
}

void ytm_prefetch_drop(const char *video_id) {
  PreSlot *slot = pre_find(video_id);
  if (!slot) return;
  slot->id[0] = 0;
  slot->ready = 0;
  slot->url[0] = 0;
}

static int http_get_hdr(const char *url, unsigned char *buf, int cap, int *status, const char *ua,
                        const char *ref);

static int http_get_bin(const char *url, unsigned char *buf, int cap, int *status) {
  return http_get_hdr(url, buf, cap, status, NULL, NULL);
}

static int starts_location(const char *p, const char *end) {
  static const char *k = "location:";
  int i;
  for (i = 0; k[i]; i++) {
    char c;
    if (p + i >= end) return 0;
    c = p[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != k[i]) return 0;
  }
  return 1;
}

static int header_location(int req, char *dst, int n) {
  char *hdr = NULL;
  size_t len = 0;
  const char *p;
  const char *e;
  dst[0] = 0;
  if (n < 8) return -1;
  if (sceHttp2GetAllResponseHeaders(req, &hdr, &len) != 0 || !hdr || len == 0) return -1;
  p = hdr;
  e = hdr + len;
  while (p < e) {
    const char *nl = p;
    int k;
    while (nl < e && *nl != '\n') nl++;
    if (starts_location(p, nl)) {
      const char *v = p + 9;
      while (v < nl && (*v == ' ' || *v == '\t')) v++;
      k = (int)(nl - v);
      while (k > 0 && (v[k - 1] == '\r' || v[k - 1] == ' ')) k--;
      if (k < 8 || k >= n) return -1;
      memcpy(dst, v, (size_t)k);
      dst[k] = 0;
      return 0;
    }
    p = nl < e ? nl + 1 : e;
  }
  return -1;
}

static int http_get_hdr(const char *url, unsigned char *buf, int cap, int *status, const char *ua,
                        const char *ref) {
  char cur[8192];
  char next[8192];
  int hop;
  if (!buf || cap < 8 || g_tmpl < 0 || !url || !url[0]) return -1;
  snprintf(cur, sizeof cur, "%s", url);
  for (hop = 0; hop < 4; hop++) {
    int req, total = 0, n, code = 0;
    req = sceHttp2CreateRequestWithURL(g_tmpl, "GET", cur, 0);
    if (req < 0) return -1;
    sceHttp2SetAutoRedirect(req, 1);
    sceHttp2AddRequestHeader(req, "Accept", "*/*", SCE_HTTP_HEADER_OVERWRITE);
    sceHttp2AddRequestHeader(req, "User-Agent", ua && ua[0] ? ua : "Mozilla/5.0",
                             SCE_HTTP_HEADER_OVERWRITE);
    if (g_cookie && g_cookie[0]) {
      sceHttp2AddRequestHeader(req, "Cookie", g_cookie, SCE_HTTP_HEADER_OVERWRITE);
      sceHttp2AddRequestHeader(req, "Origin", "https://music.youtube.com", SCE_HTTP_HEADER_OVERWRITE);
    }
    if (ref && ref[0])
      sceHttp2AddRequestHeader(req, "Referer", ref, SCE_HTTP_HEADER_OVERWRITE);
    if (sceHttp2SendRequest(req, "", 0) != 0) {
      sceHttp2DeleteRequest(req);
      return -1;
    }
    sceHttp2GetStatusCode(req, &code);
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      if (header_location(req, next, (int)sizeof next) != 0 || strncmp(next, "http", 4) != 0) {
        sceHttp2DeleteRequest(req);
        return -1;
      }
      sceHttp2DeleteRequest(req);
      snprintf(cur, sizeof cur, "%s", next);
      continue;
    }
    while (total + 1 < cap &&
           (n = sceHttp2ReadData(req, buf + total, (size_t)(cap - total - 1))) > 0)
      total += n;
    sceHttp2DeleteRequest(req);
    if (status) *status = code;
    return total;
  }
  return -1;
}

/* ytmusicapi get_visitor_id reads VISITOR_DATA out of the music homepage. */
static int ensure_music_visitor(void) {
  int status = 0;
  int n;
  const char *p;
  const char *saved;
  if (g_music_visitor[0]) return 0;
  if (!g_resp) return -1;
  saved = g_cookie;
  g_cookie = "SOCS=CAI";
  n = http_get_hdr("https://music.youtube.com/", (unsigned char *)g_resp, RESP_MAX - 1, &status,
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:88.0) Gecko/20100101 Firefox/88.0",
                   NULL);
  g_cookie = saved;
  if (n < 32 || status != 200) return -1;
  g_resp[n] = 0;
  p = strstr(g_resp, "\"VISITOR_DATA\":\"");
  if (!p) return -1;
  copy_json_str(p + 16, g_music_visitor, (int)sizeof g_music_visitor);
  if ((int)strlen(g_music_visitor) < 8 || strchr(g_music_visitor, ' ') ||
      strchr(g_music_visitor, '"') || strchr(g_music_visitor, '\\')) {
    g_music_visitor[0] = 0;
    return -1;
  }
  return 0;
}

static int lookup_region(char *cc, int n) {
  unsigned char buf[512];
  int status = 0;
  int got;
  const char *p;
  char a, b;
  if (!cc || n < 3) return -1;
  cc[0] = 0;
  if (g_place[0] && strcmp(g_place, "ZZ") != 0 && strlen(g_place) == 2) {
    snprintf(cc, (size_t)n, "%s", g_place);
    return 0;
  }
  got = http_get_hdr("https://www.cloudflare.com/cdn-cgi/trace", buf, (int)sizeof buf, &status,
                     "Mozilla/5.0", NULL);
  if (got < 8 || status != 200) return -1;
  if (got >= (int)sizeof buf) got = (int)sizeof buf - 1;
  buf[got] = 0;
  p = strstr((char *)buf, "loc=");
  if (!p) return -1;
  a = p[4];
  b = p[5];
  if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
  if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
  if (!((a >= 'A' && a <= 'Z') && (b >= 'A' && b <= 'Z'))) return -1;
  cc[0] = a;
  cc[1] = b;
  cc[2] = 0;
  return 0;
}

int ytm_stream_follow(char *url, int url_n) {
  char cur[8192];
  char next[8192];
  int hop;
  if (!url || !url[0] || url_n < 16 || g_tmpl < 0) return -1;
  snprintf(cur, sizeof cur, "%s", url);
  for (hop = 0; hop < 3; hop++) {
    int req, code = 0;
    req = sceHttp2CreateRequestWithURL(g_tmpl, "GET", cur, 0);
    if (req < 0) return -1;
    sceHttp2SetAutoRedirect(req, 0);
    sceHttp2AddRequestHeader(req, "Accept", "*/*", SCE_HTTP_HEADER_OVERWRITE);
    sceHttp2AddRequestHeader(req, "User-Agent", ytm_stream_ua(), SCE_HTTP_HEADER_OVERWRITE);
    sceHttp2AddRequestHeader(req, "Referer", ytm_stream_referer(), SCE_HTTP_HEADER_OVERWRITE);
    sceHttp2AddRequestHeader(req, "Range", "bytes=0-0", SCE_HTTP_HEADER_OVERWRITE);
    if (sceHttp2SendRequest(req, "", 0) != 0) {
      sceHttp2DeleteRequest(req);
      return -1;
    }
    sceHttp2GetStatusCode(req, &code);
    if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) &&
        header_location(req, next, (int)sizeof next) == 0 && strncmp(next, "https://", 8) == 0) {
      sceHttp2DeleteRequest(req);
      snprintf(cur, sizeof cur, "%s", next);
      continue;
    }
    /* Leave the body unread so the UI thread does not copy the song. */
    sceHttp2DeleteRequest(req);
    break;
  }
  snprintf(url, (size_t)url_n, "%s", cur);
  return 0;
}

static const char *cover_key_of(const Track *t, char *tmp, int n) {
  if (t->id[0]) snprintf(tmp, (size_t)n, "%s", t->id);
  else if (t->browse[0]) snprintf(tmp, (size_t)n, "%s", t->browse);
  else snprintf(tmp, (size_t)n, "%s", t->thumb);
  return tmp;
}

static CoverSlot *cover_find(CoverSlot *slots, int nslots, const char *key) {
  int i;
  for (i = 0; i < nslots; i++) {
    if (slots[i].key[0] && strcmp(slots[i].key, key) == 0) return &slots[i];
  }
  return NULL;
}

static CoverSlot *cover_slot(CoverSlot *slots, int nslots, const char *key) {
  CoverSlot *slot = cover_find(slots, nslots, key);
  CoverSlot *victim = NULL;
  int i;
  if (slot) return slot;
  for (i = 0; i < nslots; i++) {
    if (!slots[i].key[0]) return &slots[i];
    if (!victim || slots[i].stamp < victim->stamp) victim = &slots[i];
  }
  free(victim->px);
  victim->px = NULL;
  victim->key[0] = 0;
  victim->fails = 0;
  return victim;
}

static int try_jpeg(const char *url, unsigned char *buf, unsigned char *dst, int side) {
  int status = 0;
  int n;
  if (!url || !url[0]) return -1;
  n = http_get_bin(url, buf, 512 * 1024, &status);
  if (status == 200 && n > 64 && art_jpeg_square(buf, n, dst, side) == 0) return 0;
  return -1;
}

static int cover_fetch_pool(CoverSlot *slots, int nslots, int side, unsigned *tick, const Track *t,
                            int hero) {
  char key[80];
  char url[YTM_THUMB_LEN];
  CoverSlot *slot;
  unsigned char *buf;
  unsigned char *dst;
  int ok = 0;
  cover_key_of(t, key, (int)sizeof key);
  if (!key[0]) return -1;
  slot = cover_find(slots, nslots, key);
  if (slot && slot->px) {
    slot->stamp = ++(*tick);
    return 0;
  }
  if (slot && slot->fails >= 3) return -1;
  slot = cover_slot(slots, nslots, key);
  buf = (unsigned char *)malloc(512 * 1024);
  dst = (unsigned char *)malloc((size_t)side * (size_t)side * 4u);
  if (!buf || !dst) {
    free(buf);
    free(dst);
    return -1;
  }
  url[0] = 0;
  if (t->id[0]) {
    if (hero) {
      snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/hq720.jpg", t->id);
      if (try_jpeg(url, buf, dst, side) == 0) ok = 1;
    }
    if (!ok) {
      snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/sddefault.jpg", t->id);
      if (try_jpeg(url, buf, dst, side) == 0) ok = 1;
    }
    if (!ok) {
      snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", t->id);
      if (try_jpeg(url, buf, dst, side) == 0) ok = 1;
    }
  }
  if (!ok && t->thumb[0] && !( !hero && strstr(t->thumb, "hq720"))) {
    if (try_jpeg(t->thumb, buf, dst, side) == 0) ok = 1;
  }
  if (ok) {
    free(slot->px);
    slot->px = dst;
    slot->fails = 0;
    snprintf(slot->key, sizeof slot->key, "%s", key);
    slot->stamp = ++(*tick);
  } else {
    free(dst);
    if (!slot->key[0] || strcmp(slot->key, key) != 0) {
      snprintf(slot->key, sizeof slot->key, "%s", key);
      slot->fails = 1;
    } else if (slot->fails < 8) {
      slot->fails++;
    }
    slot->stamp = ++(*tick);
  }
  free(buf);
  return slot->px ? 0 : -1;
}

int ytm_cover_fetch(const Track *t, int side) {
  if (!t) return -1;
  if (side >= 200)
    return cover_fetch_pool(g_hero, HERO_SLOTS, HERO_SIDE, &g_hero_tick, t, 1);
  return cover_fetch_pool(g_grid, GRID_SLOTS, GRID_SIDE, &g_grid_tick, t, 0);
}

const unsigned char *ytm_cover_pixels(const Track *t, int side, int *w, int *h) {
  char key[80];
  CoverSlot *slot;
  int hero;
  if (!t) return NULL;
  cover_key_of(t, key, (int)sizeof key);
  hero = side >= 200;
  slot = cover_find(hero ? g_hero : g_grid, hero ? HERO_SLOTS : GRID_SLOTS, key);
  if (!slot || !slot->px) return NULL;
  slot->stamp = hero ? ++g_hero_tick : ++g_grid_tick;
  if (w) *w = hero ? HERO_SIDE : GRID_SIDE;
  if (h) *h = hero ? HERO_SIDE : GRID_SIDE;
  return slot->px;
}

#ifdef YTM_PARSE_TEST
int sceNetInit(void) { return 0; }
int sceNetPoolCreate(const char *a, int b, int c) {
  (void)a;
  (void)b;
  (void)c;
  return 0;
}
int sceNetPoolDestroy(int a) {
  (void)a;
  return 0;
}
int sceSslInit(size_t a) {
  (void)a;
  return 0;
}
int sceSslTerm(int a) {
  (void)a;
  return 0;
}
int sceHttp2Init(int a, int b, size_t c, int d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return 0;
}
int sceHttp2Term(int a) {
  (void)a;
  return 0;
}
int sceHttp2CreateTemplate(int a, const char *b, int c, int d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return 0;
}
int sceHttp2DeleteTemplate(int a) {
  (void)a;
  return 0;
}
int sceHttp2CreateRequestWithURL(int a, const char *b, const char *c, unsigned long long d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return 0;
}
int sceHttp2DeleteRequest(int a) {
  (void)a;
  return 0;
}
int sceHttp2AddRequestHeader(int a, const char *b, const char *c, unsigned int d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return 0;
}
int sceHttp2SendRequest(int a, const void *b, size_t c) {
  (void)a;
  (void)b;
  (void)c;
  return 0;
}
int sceHttp2GetStatusCode(int a, int *b) {
  (void)a;
  if (b) *b = 0;
  return 0;
}
int sceHttp2ReadData(int a, void *b, size_t c) {
  (void)a;
  (void)b;
  (void)c;
  return 0;
}
int sceHttp2SetAutoRedirect(int a, int b) {
  (void)a;
  (void)b;
  return 0;
}
int sceHttp2GetAllResponseHeaders(int a, char **b, size_t *c) {
  (void)a;
  if (b) *b = NULL;
  if (c) *c = 0;
  return -1;
}
int art_jpeg_square(const unsigned char *a, int b, unsigned char *c, int d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return -1;
}
int main(int argc, char **argv) {
  FILE *f;
  char *buf;
  long sz;
  Track out[40];
  int c, i;
  if (argc < 2) return 1;
  f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (char *)malloc((size_t)sz + 1);
  if (!buf) return 1;
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) return 1;
  buf[sz] = 0;
  fclose(f);
  if (strstr(buf, "\"contentDetails\"")) c = collect_api(buf, out, 40);
  else c = collect_tracks(buf, out, 40);
  printf("count %d\n", c);
  for (i = 0; i < c && i < 8; i++) {
    printf("[%d] id=%s browse=%s album=%s artist_id=%s\n  title=%s\n  artist=%s\n  album_name=%s\n  sec=%d thumb=%s\n",
           i, out[i].id, out[i].browse, out[i].album_id, out[i].artist_id, out[i].title,
           out[i].artist, out[i].album, out[i].seconds, out[i].thumb);
  }
  free(buf);
  return c > 0 ? 0 : 2;
}
#endif
