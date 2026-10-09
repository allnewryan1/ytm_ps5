#include "net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int http_post_ex(const char *url, const char *body, const char *ua, const char *origin,
                         const char *referer, const char *client_name, const char *client_ver,
                         int *status, char *err, int err_n) {
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
  if (*status != 200) {
    snprintf(err, (size_t)err_n, "YouTube HTTP %d", *status);
    return -1;
  }
  if (total < 2) {
    snprintf(err, (size_t)err_n, "Empty response");
    return -1;
  }
  err[0] = 0;
  return total;
}

static int http_post(const char *url, const char *body, int *status, char *err, int err_n) {
  return http_post_ex(url, body, NULL, "https://music.youtube.com", "https://music.youtube.com/",
                      NULL, NULL, status, err, err_n);
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

int ytm_audio_url(const char *video_id, char *url, int url_n, int *duration,
                  char *err, int err_n) {
  /* visionOS Innertube still returns a direct AAC URL without a proof-of-origin
   * token. Android and iOS clients now answer with a cipher or a sign-in wall. */
  static const char *ua =
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/26.0 Safari/605.1.15";
  char body[1024];
  char endpoint[320];
  int status = 0;
  int len = 0;
  *duration = 0;
  url[0] = 0;
  snprintf(endpoint, sizeof endpoint,
           "https://www.youtube.com/youtubei/v1/player?prettyPrint=false");
  snprintf(body, sizeof body,
           "{\"context\":{\"client\":{\"clientName\":\"VISIONOS\","
           "\"clientVersion\":\"1.02\",\"deviceMake\":\"Apple\","
           "\"deviceModel\":\"RealityDevice17,1\",\"osName\":\"visionOS\","
           "\"osVersion\":\"26.5.23O471\",\"hl\":\"en\",\"gl\":\"US\","
           "\"userAgent\":\"%s\"}},\"videoId\":\"%s\","
           "\"contentCheckOk\":true,\"racyCheckOk\":true}",
           ua, video_id);
  if (http_post_ex(endpoint, body, ua, "https://www.youtube.com", "https://www.youtube.com/",
                   "101", "1.02", &status, err, err_n) < 0) {
    return -1;
  }
  json_int(g_resp, "lengthSeconds", &len);
  if (len <= 0) {
    int ms = 0;
    if (json_int(g_resp, "approxDurationMs", &ms) && ms > 0) len = ms / 1000;
  }
  if (pick_audio_url(g_resp, url, url_n)) {
    *duration = len;
    err[0] = 0;
    return 0;
  }
  if (strstr(g_resp, "signatureCipher") || strstr(g_resp, "\"signature\":\"")) {
    snprintf(err, (size_t)err_n, "YouTube signed this stream; no direct audio URL");
  } else if (strstr(g_resp, "LOGIN_REQUIRED")) {
    snprintf(err, (size_t)err_n, "YouTube asked this console to sign in");
  } else {
    char reason[160];
    if (json_string(g_resp, "reason", reason, (int)sizeof reason))
      snprintf(err, (size_t)err_n, "%s", reason);
    else
      snprintf(err, (size_t)err_n, "No audio URL in the player response");
  }
  return -1;
}
