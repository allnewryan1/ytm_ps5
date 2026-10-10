/* Audio probe v3: which sceAudioOut path is audible, from a loader payload, on the home screen
 * and while a game has focus, before and after the payload raises its own privileges?
 *
 * v1 and v2 findings: every method was silent in a game, methods 3-4 failed to open, and the
 * log could not be written to /data. A loader payload that cannot write /data is still jailed
 * with a sandboxed process's credentials, which may also be why it is not heard. ps5upload's
 * helper raises its credentials and leaves the jail at start for the same reason.
 *
 * Send it from the home screen and stay there:
 *   1. "Home A": method 1 as started (4 s tone).
 *   2. It raises its privileges (needs kstuff) and says whether that worked.
 *   3. "Home B" and "Home C": methods 1 and 2, elevated (4 s each).
 *   4. "Open a game now": 45 s, then two rounds of six methods in the game, elevated. Each
 *      plays its number as beeps, then a 6 s tone.
 *   5. A notification with every result. The log goes to /data/ytmusic/audioprobe.log once
 *      /data is writable, and to stdout (the sender's terminal, if it keeps the connection). */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutSysOpen(int user, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *buf);
int sceAudioOutClose(int handle);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user);

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

#define USER_SYSTEM 0xFF
#define PORT_MAIN 0
#define PORT_BGM 1
#define FMT_S16_STEREO 1
#define RATE 48000
#define GRAIN 256
#define DEBUGGER_AUTHID 0x4800000000000006ull /* ps5upload's default; not ShellCore's */

/* Everything logged is kept in memory too, so it reaches the file once /data opens up. */
static char g_mem[32768];
static size_t g_mem_n;
static FILE *g_log;
static char g_summary[2600];

static void note(const char *fmt, ...) {
  char line[512];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n >= (int)sizeof line) n = (int)sizeof line - 1;
  printf("audioprobe: %s\n", line);
  fflush(stdout);
  if (g_mem_n + (size_t)n + 2 < sizeof g_mem) {
    memcpy(g_mem + g_mem_n, line, (size_t)n);
    g_mem_n += (size_t)n;
    g_mem[g_mem_n++] = '\n';
  }
  if (g_log) {
    fprintf(g_log, "%s\n", line);
    fflush(g_log);
  }
}

static void say(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;
  memset(&req, 0, sizeof req);
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  note("[toast] %s", req.message);
}

static void sum(const char *fmt, ...) {
  size_t used = strlen(g_summary);
  va_list ap;
  if (used + 2 >= sizeof g_summary) return;
  va_start(ap, fmt);
  vsnprintf(g_summary + used, sizeof g_summary - used, fmt, ap);
  va_end(ap);
}

/* 1 if a file can be created in /data/ytmusic. Opens the log the first time it can. */
static int data_writable(void) {
  FILE *f;
  mkdir("/data/ytmusic", 0755);
  if (g_log) return 1;
  f = fopen("/data/ytmusic/audioprobe.log", "w");
  if (!f) return 0;
  g_log = f;
  fwrite(g_mem, 1, g_mem_n, g_log);
  fflush(g_log);
  return 1;
}

static void creds(const char *when) {
  pid_t pid = getpid();
  intptr_t root = kernel_get_root_vnode();
  intptr_t rd = kernel_get_proc_rootdir(pid);
  note("%s: pid %d uid %d authid 0x%016llx rootdir %s, /data writable %s", when, (int)pid,
       (int)kernel_get_ucred_uid(pid), (unsigned long long)kernel_get_ucred_authid(pid),
       rd && rd == root ? "is the real root" : "is a jail", data_writable() ? "yes" : "no");
}

/* ps5upload's helper escalation, step by step. 0 when every step took. */
static int elevate(void) {
  pid_t pid = getpid();
  uint8_t caps[16], attrs[32];
  intptr_t root = kernel_get_root_vnode();
  int bad = 0;
  bad |= kernel_set_ucred_uid(pid, 0) != 0;
  bad |= kernel_set_ucred_ruid(pid, 0) != 0;
  bad |= kernel_set_ucred_svuid(pid, 0) != 0;
  bad |= kernel_set_ucred_rgid(pid, 0) != 0;
  bad |= kernel_set_ucred_svgid(pid, 0) != 0;
  if (root) {
    bad |= kernel_set_proc_rootdir(pid, root) != 0;
    bad |= kernel_set_proc_jaildir(pid, root) != 0;
  } else {
    bad = 1;
  }
  bad |= kernel_set_ucred_authid(pid, DEBUGGER_AUTHID) != 0;
  memset(caps, 0xff, sizeof caps);
  bad |= kernel_set_ucred_caps(pid, caps) != 0;
  memset(attrs, 0, sizeof attrs);
  attrs[0] = 0x80;
  bad |= kernel_set_ucred_attrs(pid, attrs) != 0;
  return bad ? -1 : 0;
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Play `ms` of a sine at `hz` (0 = silence). Returns the first output error, or 0. */
static int play(int h, double hz, int ms) {
  static int16_t buf[GRAIN * 2];
  static double phase;
  int grains = (RATE / GRAIN) * ms / 1000;
  for (int g = 0; g < grains; g++) {
    for (int i = 0; i < GRAIN; i++) {
      int16_t v = 0;
      if (hz > 0) {
        v = (int16_t)(9000.0 * sin(phase));
        phase += 2.0 * M_PI * hz / RATE;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
      }
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    int rc = sceAudioOutOutput(h, buf);
    if (rc < 0) return rc;
  }
  return 0;
}

typedef struct {
  const char *name;
  int sys;     /* sceAudioOutSysOpen instead of sceAudioOutOpen */
  int fg_user; /* foreground user instead of the system user */
  int port;
} Method;

static const Method methods[] = {
    {"MAIN port, system user (what the app uses now)", 0, 0, PORT_MAIN},
    {"BGM port, system user", 0, 0, PORT_BGM},
    {"MAIN port, your user", 0, 1, PORT_MAIN},
    {"BGM port, your user", 0, 1, PORT_BGM},
    {"SysOpen MAIN port, system user", 1, 0, PORT_MAIN},
    {"SysOpen BGM port, system user", 1, 0, PORT_BGM},
};
#define NMETHODS (int)(sizeof methods / sizeof methods[0])

static int open_method(int m, int fg) {
  const Method *me = &methods[m];
  int user = me->fg_user ? fg : USER_SYSTEM;
  return me->sys ? sceAudioOutSysOpen(user, me->port, 0, GRAIN, RATE, FMT_S16_STEREO)
                 : sceAudioOutOpen(user, me->port, 0, GRAIN, RATE, FMT_S16_STEREO);
}

/* Open method m, play its beeps (0 = none) and a tone, close. Logs and summarizes. */
static void test(const char *label, int m, int fg, int beeps, int tone_ms) {
  long long t0;
  int rc = 0;
  int h = open_method(m, fg);
  if (h < 0) {
    note("%s method %d (%s): open failed 0x%08x", label, m + 1, methods[m].name, (unsigned)h);
    sum("%s m%d open %08x\n", label, m + 1, (unsigned)h);
    return;
  }
  rc = play(h, 0, 600);
  for (int b = 0; b < beeps && rc == 0; b++) {
    rc = play(h, 880, 180);
    if (rc == 0) rc = play(h, 0, 220);
  }
  if (rc == 0) rc = play(h, 0, 400);
  t0 = now_ms();
  if (rc == 0) rc = play(h, 440, tone_ms);
  note("%s method %d (%s): %d ms tone took %lld ms, output 0x%08x", label, m + 1, methods[m].name,
       tone_ms, now_ms() - t0, (unsigned)rc);
  sum("%s m%d %lldms%s\n", label, m + 1, now_ms() - t0, rc ? " err" : "");
  play(h, 0, 300);
  sceAudioOutClose(h);
}

int main(void) {
  int fg = -1;
  int rc;

  creds("as started");
  rc = sceUserServiceInitialize(NULL);
  note("sceUserServiceInitialize: 0x%08x", (unsigned)rc);
  rc = sceUserServiceGetForegroundUser(&fg);
  note("sceUserServiceGetForegroundUser: 0x%08x user=%d", (unsigned)rc, fg);
  rc = sceAudioOutInit();
  note("sceAudioOutInit: 0x%08x", (unsigned)rc);

  say("Audio probe v3. Home A: a 4 second tone, before raising privileges.");
  sleep(2);
  test("homeA", 0, fg, 0, 4000);

  rc = elevate();
  creds("after elevating");
  say("Privileges %s. /data %s. Home B and C next.", rc == 0 ? "raised" : "NOT fully raised",
      data_writable() ? "writable" : "still not writable");
  sum("elevate %s, /data %s\n", rc == 0 ? "ok" : "FAILED", data_writable() ? "rw" : "ro");
  sleep(2);
  say("Home B: method 1, elevated.");
  test("homeB", 0, fg, 0, 4000);
  sleep(1);
  say("Home C: method 2 (BGM), elevated.");
  test("homeC", 1, fg, 0, 4000);

  say("Audio probe: open a game now. Tests start in 45 seconds.");
  sleep(45);
  for (int round = 1; round <= 2; round++) {
    for (int m = 0; m < NMETHODS; m++) {
      char label[16];
      if (methods[m].fg_user && fg < 0) continue;
      snprintf(label, sizeof label, "game%d", round);
      say("Probe %d of %d: %s", m + 1, NMETHODS, methods[m].name);
      test(label, m, fg, m + 1, 6000);
      sleep(2);
    }
  }
  say("Audio probe done.\n%s", g_summary);
  if (g_log) fclose(g_log);
  return 0;
}
