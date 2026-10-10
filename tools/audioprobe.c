/* Audio probe: which sceAudioOut path stays audible while a game has focus?
 *
 * v2. Send this to the ELF loader (port 9021) from the home screen and stay there.
 * Phase 1, on the home screen: methods 1 and 2 play a 4-second tone each.
 * Then it asks you to open a game within 45 seconds.
 * Phase 2, in the game: three rounds of six methods. Each plays its number as
 * short beeps, then a 6-second tone, and shows a notification naming it.
 * Return codes and how long each tone took to play go to /data/ytmusic/audioprobe.log:
 * a port that really plays blocks for the tone's length; one that discards returns early.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

static FILE *g_log;

static void say(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;
  memset(&req, 0, sizeof req);
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  printf("audioprobe: %s\n", req.message);
  if (g_log) {
    fprintf(g_log, "%s\n", req.message);
    fflush(g_log);
  }
}

static void note(const char *fmt, ...) {
  va_list ap;
  if (!g_log) return;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
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

static int open_method(const Method *me, int fg) {
  int user = me->fg_user ? fg : USER_SYSTEM;
  return me->sys ? sceAudioOutSysOpen(user, me->port, 0, GRAIN, RATE, FMT_S16_STEREO)
                 : sceAudioOutOpen(user, me->port, 0, GRAIN, RATE, FMT_S16_STEREO);
}

/* Tone for ms, logged with how long the port took to accept it. */
static int timed_tone(int h, const char *label, int ms) {
  long long t0 = now_ms();
  int rc = play(h, 440, ms);
  note("%s: %d ms tone took %lld ms, output -> 0x%08x", label, ms, now_ms() - t0, (unsigned)rc);
  return rc;
}

int main(void) {
  int fg = -1;
  int rc;
  mkdir("/data/ytmusic", 0755);
  g_log = fopen("/data/ytmusic/audioprobe.log", "w");
  rc = sceUserServiceInitialize(NULL);
  note("sceUserServiceInitialize: 0x%08x", (unsigned)rc);
  rc = sceUserServiceGetForegroundUser(&fg);
  note("sceUserServiceGetForegroundUser: 0x%08x user=%d", (unsigned)rc, fg);
  rc = sceAudioOutInit();
  note("sceAudioOutInit: 0x%08x", (unsigned)rc);

  /* Phase 1: on the home screen. Can this process be heard at all? */
  for (int m = 0; m < 2; m++) {
    char label[64];
    int h = open_method(&methods[m], fg);
    snprintf(label, sizeof label, "home method %d", m + 1);
    note("%s (%s): open -> 0x%08x", label, methods[m].name, (unsigned)h);
    if (h < 0) continue;
    say("Home screen test %d of 2: %s", m + 1, methods[m].name);
    play(h, 0, 500);
    timed_tone(h, label, 4000);
    play(h, 0, 300);
    sceAudioOutClose(h);
    sleep(1);
  }

  say("Audio probe: open a game now. Tests start in 45 seconds.");
  sleep(45);

  for (int round = 1; round <= 3; round++) {
    for (int m = 0; m < NMETHODS; m++) {
      const Method *me = &methods[m];
      int user = me->fg_user ? fg : USER_SYSTEM;
      int h;
      if (me->fg_user && fg < 0) {
        note("round %d method %d: skipped, no foreground user", round, m + 1);
        continue;
      }
      h = open_method(me, fg);
      note("round %d method %d (%s): open user=%d -> 0x%08x", round, m + 1, me->name, user,
           (unsigned)h);
      if (h < 0) {
        say("Probe %d of %d: %s. Open failed 0x%08x", m + 1, NMETHODS, me->name, (unsigned)h);
        sleep(3);
        continue;
      }
      say("Probe %d of %d: %s", m + 1, NMETHODS, me->name);
      rc = play(h, 0, 800);
      for (int b = 0; b <= m && rc == 0; b++) {
        rc = play(h, 880, 180);
        if (rc == 0) rc = play(h, 0, 220);
      }
      if (rc == 0) rc = play(h, 0, 500);
      if (rc == 0) {
        char label[64];
        snprintf(label, sizeof label, "round %d method %d", round, m + 1);
        rc = timed_tone(h, label, 6000);
      }
      if (rc == 0) rc = play(h, 0, 300);
      note("round %d method %d: output -> 0x%08x", round, m + 1, (unsigned)rc);
      note("round %d method %d: close -> 0x%08x", round, m + 1, (unsigned)sceAudioOutClose(h));
      sleep(2);
    }
  }
  say("Audio probe done. Log: /data/ytmusic/audioprobe.log");
  if (g_log) fclose(g_log);
  return 0;
}
