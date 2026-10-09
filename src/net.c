#include "net.h"

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

#define SCE_HTTP_HEADER_OVERWRITE 0u

/* Public web innertube key shipped in the YouTube Music page itself. */
static const char *INNERTUBE_KEY = "AIzaSyC9XL3ZjWddXya6X74dJoCTL-WEYFDNX30";

#define RESP_MAX (1536 * 1024)

static int g_net = -1;
static int g_ssl = -1;
static int g_http = -1;
static int g_tmpl = -1;
static char *g_resp;

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void copy_json_str(const char *src, char *dst, int n) {
  int o = 0;
  if (n <= 0) return;
  while (*src && *src != '"') {
    unsigned char ch;
    if (*src == '\\') {
      src++;
      if (*src == 'u' && src[1] && src[2] && src[3] && src[4]) {
        int h1 = hex_nibble(src[1]);
        int h2 = hex_nibble(src[2]);
        int h3 = hex_nibble(src[3]);
        int h4 = hex_nibble(src[4]);
        src += 5;
        if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) ch = '?';
        else {
          int cp = (h1 << 12) | (h2 << 8) | (h3 << 4) | h4;
          ch = (cp >= 0 && cp < 128) ? (unsigned char)cp : (unsigned char)'?';
        }
      } else if (*src == 0) {
        break;
      } else if (*src == 'n') {
        ch = ' ';
        src++;
      } else if (*src == 't') {
        ch = ' ';
        src++;
      } else {
        ch = (unsigned char)*src++;
      }
    } else {
      ch = (unsigned char)*src++;
    }
    if (ch < 32) ch = ' ';
    if (o + 1 < n) dst[o++] = (char)ch;
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
      "views", "VIDEO", "Episode", "Sign in", NULL};
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
  err[0] = 0;
  return 0;
}

void net_shutdown(void) {
  if (g_tmpl >= 0) sceHttp2DeleteTemplate(g_tmpl);
  if (g_http >= 0) sceHttp2Term(g_http);
  if (g_ssl >= 0) sceSslTerm(g_ssl);
  if (g_net >= 0) sceNetPoolDestroy(g_net);
  free(g_resp);
  g_resp = NULL;
  g_tmpl = g_http = g_ssl = g_net = -1;
}

static char g_access[2048];
static char g_refresh[1024];
static char g_device_code[256];
static char g_visitor[128];
static time_t g_access_exp;

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
  sceHttp2AddRequestHeader(req, "Accept", "application/json", SCE_HTTP_HEADER_OVERWRITE);
  if (ua) sceHttp2AddRequestHeader(req, "User-Agent", ua, SCE_HTTP_HEADER_OVERWRITE);
  if (origin) sceHttp2AddRequestHeader(req, "Origin", origin, SCE_HTTP_HEADER_OVERWRITE);
  if (referer) sceHttp2AddRequestHeader(req, "Referer", referer, SCE_HTTP_HEADER_OVERWRITE);
  if (client_name)
    sceHttp2AddRequestHeader(req, "X-YouTube-Client-Name", client_name, SCE_HTTP_HEADER_OVERWRITE);
  if (client_ver)
    sceHttp2AddRequestHeader(req, "X-YouTube-Client-Version", client_ver, SCE_HTTP_HEADER_OVERWRITE);
  if (g_visitor[0])
    sceHttp2AddRequestHeader(req, "X-Goog-Visitor-Id", g_visitor, SCE_HTTP_HEADER_OVERWRITE);
  if (authed && g_access[0]) {
    char auth[2100];
    snprintf(auth, sizeof auth, "Bearer %s", g_access);
    sceHttp2AddRequestHeader(req, "Authorization", auth, SCE_HTTP_HEADER_OVERWRITE);
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

static int collect_tracks(const char *json, Track *out, int max) {
  const char *p = json;
  int count = 0;
  while (count < max && (p = strstr(p, "\"videoId\":\"")) != NULL) {
    char id[16];
    char labels[6][YTM_TITLE_LEN];
    int nlab = 0;
    const char *at = p;
    const char *q;
    const char *base;
    p += 11;
    if ((int)strlen(p) < 12 || p[11] != '"') {
      p = at + 11;
      continue;
    }
    memcpy(id, p, 11);
    id[11] = 0;
    p += 12;
    if (!id_ok(id)) continue;
    for (int i = 0; i < count; i++) {
      if (strcmp(out[i].id, id) == 0) {
        id[0] = 0;
        break;
      }
    }
    if (!id[0]) continue;

    base = json;
    if (at - json > 1600) base = at - 1600;
    q = base;
    while (nlab < 6 && (q = strstr(q, "\"text\":\"")) != NULL && q < at) {
      char tmp[YTM_TITLE_LEN];
      q += 8;
      copy_json_str(q, tmp, (int)sizeof tmp);
      if (!junk_label(tmp) && strlen(tmp) > 1) {
        snprintf(labels[nlab], sizeof labels[nlab], "%s", tmp);
        nlab++;
      }
    }

    memset(&out[count], 0, sizeof out[count]);
    memcpy(out[count].id, id, sizeof out[count].id);
    out[count].seconds = 0;
    if (nlab >= 2 && clock_seconds(labels[nlab - 1]) >= 0) {
      out[count].seconds = clock_seconds(labels[nlab - 1]);
      nlab--;
    }
    if (nlab >= 2) {
      snprintf(out[count].title, sizeof out[count].title, "%s", labels[nlab - 2]);
      snprintf(out[count].artist, sizeof out[count].artist, "%s", labels[nlab - 1]);
    } else if (nlab == 1) {
      snprintf(out[count].title, sizeof out[count].title, "%s", labels[0]);
      snprintf(out[count].artist, sizeof out[count].artist, "%s", "YouTube Music");
    } else {
      snprintf(out[count].title, sizeof out[count].title, "%s", id);
      snprintf(out[count].artist, sizeof out[count].artist, "%s", "YouTube Music");
    }
    count++;
  }
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

static int browse_tracks(const char *host, const char *browse_id, Track *out, int max, char *err,
                         int err_n) {
  char body[512];
  char url[192];
  int status = 0;
  int n;
  snprintf(url, sizeof url, "https://%s/youtubei/v1/browse?prettyPrint=false", host);
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\","
           "\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"US\"}},"
           "\"browseId\":\"%s\"}",
           TV_VER, browse_id);
  if (http_post_ex(url, body, TV_UA, "https://www.youtube.com", "https://www.youtube.com/tv", "7",
                   TV_VER, 1, 0, &status, err, err_n) < 0)
    return -1;
  n = collect_tracks(g_resp, out, max);
  if (n == 0) snprintf(err, (size_t)err_n, "No songs in that library");
  return n;
}

int ytm_liked(Track *out, int max, char *err, int err_n) {
  int n;
  if (ensure_access(err, err_n) != 0) return -1;
  n = browse_tracks("music.youtube.com", "FEmusic_liked_videos", out, max, err, err_n);
  if (n > 0) return n;
  n = browse_tracks("www.youtube.com", "VLLM", out, max, err, err_n);
  return n;
}
