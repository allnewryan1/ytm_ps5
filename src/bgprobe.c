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

/* Imported, but never called blind: an import the app process is not given stays a NULL GOT
 * slot, and calling it jumps to 0 (the first test crashed exactly so). got() reads the slot
 * itself, in asm so the compiler cannot assume a function's address is non-NULL.
 * sceKernelDlsym is no help: in the app it finds nothing, not even functions SDL calls fine.
 * The BGM calls take no arguments as far as anything public shows (shadPS4 stubs them so). */
int sceUserServiceGetForegroundUser(int *user);
int sceShellCoreUtilIsBgmPlaying(void);
int sceSystemServiceIsBgmCpuBudgetAvailable(void);
int sceSystemStateMgrEnterMediaPlaybackMode(void);
int sceSystemStateMgrLeaveMediaPlaybackMode(void);
int sceSystemStateMgrTickMusicPlayback(void);
/* The BGM ("background music") budget: what a media app asks for before the system starts its
 * music core. Arguments unknown; probed with 0 and 1. */
int sceSystemServiceAcquireBgmCpuBudget(int type);
int sceShellCoreUtilIsBgmCpuBudgetAcquired(void);

#define GOT(sym)                                                     \
  ({                                                                 \
    void *slot_;                                                     \
    __asm__ volatile("movq " #sym "@GOTPCREL(%%rip), %0" : "=r"(slot_)); \
    slot_;                                                           \
  })

static int (*p_fg_user)(int *user);
static int (*p_bgm_playing)(void);
static int (*p_bgm_budget)(void);
static int (*p_enter_media)(void);
static int (*p_leave_media)(void);
static int (*p_tick_music)(void);
static int (*p_acquire)(int);
static int (*p_acquired)(void);

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

#define USER_SYSTEM 0xFF
#define PORT_MAIN 0
#define PORT_BGM 1
#define PORT_VOICE 2
#define PORT_PERSONAL 3
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

static void *check(const char *name, void *addr) {
  note("  %s: %s (%p)", name, addr ? "bound" : "not given to the app", addr);
  sum("%s %s\n", addr ? "+" : "-", name);
  return addr;
}

static void resolve(void) {
  p_fg_user = (int (*)(int *))check("sceUserServiceGetForegroundUser",
                                    GOT(sceUserServiceGetForegroundUser));
  p_bgm_playing =
      (int (*)(void))check("sceShellCoreUtilIsBgmPlaying", GOT(sceShellCoreUtilIsBgmPlaying));
  p_bgm_budget = (int (*)(void))check("sceSystemServiceIsBgmCpuBudgetAvailable",
                                      GOT(sceSystemServiceIsBgmCpuBudgetAvailable));
  p_enter_media = (int (*)(void))check("sceSystemStateMgrEnterMediaPlaybackMode",
                                       GOT(sceSystemStateMgrEnterMediaPlaybackMode));
  p_leave_media = (int (*)(void))check("sceSystemStateMgrLeaveMediaPlaybackMode",
                                       GOT(sceSystemStateMgrLeaveMediaPlaybackMode));
  p_tick_music = (int (*)(void))check("sceSystemStateMgrTickMusicPlayback",
                                      GOT(sceSystemStateMgrTickMusicPlayback));
  p_acquire = (int (*)(int))check("sceSystemServiceAcquireBgmCpuBudget",
                                  GOT(sceSystemServiceAcquireBgmCpuBudget));
  p_acquired = (int (*)(void))check("sceShellCoreUtilIsBgmCpuBudgetAcquired",
                                    GOT(sceShellCoreUtilIsBgmCpuBudgetAcquired));
}

static int bgm_playing(void) { return p_bgm_playing ? p_bgm_playing() : -1; }
static int bgm_budget(void) { return p_bgm_budget ? p_bgm_budget() : -1; }

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
    if (tick && p_tick_music && g % 187 == 0) p_tick_music();
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
    {"VOICE port, your user", 1, PORT_VOICE, 0},
    {"PERSONAL port, your user", 1, PORT_PERSONAL, 0},
};
#define NMETHODS (int)(sizeof methods / sizeof methods[0])

static void run_one(int round, int m, int fg) {
  const Method *me = &methods[m];
  int user = me->fg_user ? fg : USER_SYSTEM;
  int h, rc = 0;
  long long t0;
  if (me->fg_user && fg < 0) {
    note("round %d method %d: skipped, no foreground user", round, m + 1);
    sum("r%d m%d skip\n", round, m + 1);
    return;
  }
  if (me->media_mode && (!p_enter_media || !p_leave_media)) {
    note("round %d method %d: skipped, media playback mode not available", round, m + 1);
    sum("r%d m%d skip\n", round, m + 1);
    return;
  }
  note("round %d method %d: opening user=%d port=%d", round, m + 1, user, me->port);
  if (me->media_mode) {
    rc = p_enter_media();
    note("round %d method %d: EnterMediaPlaybackMode -> 0x%08x", round, m + 1, (unsigned)rc);
  }
  h = sceAudioOutOpen(user, me->port, 0, GRAIN, RATE, FMT_S16_STEREO);
  if (h < 0) {
    note("round %d method %d (%s): open user=%d failed 0x%08x", round, m + 1, me->name, user,
         (unsigned)h);
    sum("r%d m%d open %08x\n", round, m + 1, (unsigned)h);
    say("BG test %d of %d: %s. Open failed 0x%08x", m + 1, NMETHODS, me->name, (unsigned)h);
    if (me->media_mode) p_leave_media();
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
       round, m + 1, me->name, now_ms() - t0, (unsigned)rc, bgm_playing(), bgm_budget());
  sum("r%d m%d %lldms%s\n", round, m + 1, now_ms() - t0, rc ? " err" : "");
  play(h, 0, 300, 0);
  sceAudioOutClose(h);
  if (me->media_mode) {
    rc = p_leave_media();
    note("round %d method %d: LeaveMediaPlaybackMode -> 0x%08x", round, m + 1, (unsigned)rc);
  }
  sleep(2);
}

/* v4: Spotify's music core is started by the system right after its app asks for something;
 * the BGM CPU budget is the likeliest request. Ask for it and see whether eboot2.bin starts
 * (it posts its own "ytmcore" notifications). The tone tests are kept for later rounds. */
static void *probe_main(void *arg) {
  int rc;
  (void)arg;
  mkdir("/data/ytmusic", 0777);
  g_log = fopen("/data/ytmusic/bgprobe.log", "w");
  g_sum[0] = 0;
  note("bgprobe v4 started");
  resolve();
  note("before: budget available %d, acquired %d", bgm_budget(),
       p_acquired ? p_acquired() : -1);
  if (p_acquire) {
    rc = p_acquire(0);
    note("AcquireBgmCpuBudget(0) -> 0x%08x", (unsigned)rc);
    sum("acquire(0) %08x\n", (unsigned)rc);
    if (rc < 0) {
      rc = p_acquire(1);
      note("AcquireBgmCpuBudget(1) -> 0x%08x", (unsigned)rc);
      sum("acquire(1) %08x\n", (unsigned)rc);
    }
  }
  sleep(2);
  note("after: budget available %d, acquired %d", bgm_budget(), p_acquired ? p_acquired() : -1);
  sum("acquired now %d\n", p_acquired ? p_acquired() : -1);
  say("BG test v4: asked for the background music budget.\n%sWatch for ytmcore notifications.",
      g_sum);
  if (g_log) fclose(g_log);
  g_log = NULL;
  g_running = 0;
  (void)run_one;
  return NULL;
}

/* Created the way player.c creates its decoder thread, which is known to work here. */
int bgprobe_start(void) {
  pthread_t t;
  if (g_running) return -1;
  g_running = 1;
  if (pthread_create(&t, NULL, probe_main, NULL) != 0) {
    g_running = 0;
    return -1;
  }
  pthread_detach(t);
  return 0;
}

int bgprobe_running(void) { return g_running; }
