/* Background audio test, run from the Account page.
 *
 * Found so far: a loader payload is never mixed in (every AudioOut port accepts audio in real
 * time and stays silent). The app declared as a media app keeps running on the home screen,
 * but its audio (SDL's MAIN port, system user) is muted as soon as it loses focus. This tries,
 * from inside the app, which output stays audible while it is in the background.
 *
 * The user starts it, then goes to the home screen. Each method shows a notification, plays its
 * number as beeps and a 6 s tone. Results go to /data/ytmusic/bgprobe.log and a final notice. */

#include "bgprobe.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *buf);
int sceAudioOutClose(int handle);
int sceUserServiceGetForegroundUser(int *user);
/* These take no arguments as far as anything public shows (shadPS4 stubs them without any).
 * Only the read-only queries and the media-playback pair are used, last. */
int sceShellCoreUtilIsBgmPlaying(void);
int sceSystemServiceIsBgmCpuBudgetAvailable(void);
int sceSystemStateMgrEnterMediaPlaybackMode(void);
int sceSystemStateMgrLeaveMediaPlaybackMode(void);
int sceSystemStateMgrTickMusicPlayback(void);

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
static char g_sum[1800];
static volatile int g_running;

static void note(const char *fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  printf("bgprobe: %s\n", line);
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
  size_t used = strlen(g_sum);
  va_list ap;
  if (used + 2 >= sizeof g_sum) return;
  va_start(ap, fmt);
  vsnprintf(g_sum + used, sizeof g_sum - used, fmt, ap);
  va_end(ap);
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int play(int h, double hz, int ms, int tick) {
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
    /* About once a second, the way a video player ticks to keep the console awake. */
    if (tick && g % 187 == 0) sceSystemStateMgrTickMusicPlayback();
  }
  return 0;
}

typedef struct {
  const char *name;
  int fg_user;
  int port;
  int media_mode;
} Method;

static const Method methods[] = {
    {"MAIN port, system user (what the app uses now)", 0, PORT_MAIN, 0},
    {"BGM port, system user", 0, PORT_BGM, 0},
    {"MAIN port, your user", 1, PORT_MAIN, 0},
    {"BGM port, your user", 1, PORT_BGM, 0},
    {"BGM port, your user, media playback mode", 1, PORT_BGM, 1},
};
#define NMETHODS (int)(sizeof methods / sizeof methods[0])

static void run_one(int round, int m, int fg) {
  const Method *me = &methods[m];
  int user = me->fg_user ? fg : USER_SYSTEM;
  int h, rc = 0;
  long long t0;
  if (me->media_mode) {
    rc = sceSystemStateMgrEnterMediaPlaybackMode();
    note("round %d method %d: EnterMediaPlaybackMode -> 0x%08x", round, m + 1, (unsigned)rc);
  }
  h = sceAudioOutOpen(user, me->port, 0, GRAIN, RATE, FMT_S16_STEREO);
  if (h < 0) {
    note("round %d method %d (%s): open user=%d failed 0x%08x", round, m + 1, me->name, user,
         (unsigned)h);
    sum("r%d m%d open %08x\n", round, m + 1, (unsigned)h);
    say("BG test %d of %d: %s. Open failed 0x%08x", m + 1, NMETHODS, me->name, (unsigned)h);
    if (me->media_mode) sceSystemStateMgrLeaveMediaPlaybackMode();
    sleep(3);
    return;
  }
  say("BG test %d of %d: %s", m + 1, NMETHODS, me->name);
  rc = play(h, 0, 700, me->media_mode);
  for (int b = 0; b <= m && rc == 0; b++) {
    rc = play(h, 880, 180, me->media_mode);
    if (rc == 0) rc = play(h, 0, 220, me->media_mode);
  }
  if (rc == 0) rc = play(h, 0, 400, me->media_mode);
  t0 = now_ms();
  if (rc == 0) rc = play(h, 440, 6000, me->media_mode);
  note("round %d method %d (%s): 6000 ms tone took %lld ms, output 0x%08x; bgm playing %d, "
       "bgm budget %d",
       round, m + 1, me->name, now_ms() - t0, (unsigned)rc, sceShellCoreUtilIsBgmPlaying(),
       sceSystemServiceIsBgmCpuBudgetAvailable());
  sum("r%d m%d %lldms%s\n", round, m + 1, now_ms() - t0, rc ? " err" : "");
  play(h, 0, 300, 0);
  sceAudioOutClose(h);
  if (me->media_mode) {
    rc = sceSystemStateMgrLeaveMediaPlaybackMode();
    note("round %d method %d: LeaveMediaPlaybackMode -> 0x%08x", round, m + 1, (unsigned)rc);
  }
  sleep(2);
}

static void *probe_main(void *arg) {
  int fg = -1;
  int rc;
  (void)arg;
  mkdir("/data/ytmusic", 0777);
  g_log = fopen("/data/ytmusic/bgprobe.log", "w");
  g_sum[0] = 0;
  rc = sceUserServiceGetForegroundUser(&fg);
  note("foreground user 0x%08x user=%d; AudioOutInit 0x%08x", (unsigned)rc, fg,
       (unsigned)sceAudioOutInit());
  note("before: bgm playing %d, bgm budget %d", sceShellCoreUtilIsBgmPlaying(),
       sceSystemServiceIsBgmCpuBudgetAvailable());
  say("Background audio test: press PS and go to the home screen now. Tones start in 15 seconds.");
  sleep(15);
  for (int round = 1; round <= 2; round++)
    for (int m = 0; m < NMETHODS; m++) run_one(round, m, fg);
  say("Background audio test done.\n%s", g_sum);
  if (g_log) fclose(g_log);
  g_log = NULL;
  g_running = 0;
  return NULL;
}

int bgprobe_start(void) {
  pthread_t t;
  pthread_attr_t at;
  if (g_running) return -1;
  g_running = 1;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if (pthread_create(&t, &at, probe_main, NULL) != 0) {
    g_running = 0;
    pthread_attr_destroy(&at);
    return -1;
  }
  pthread_attr_destroy(&at);
  return 0;
}

int bgprobe_running(void) { return g_running; }
