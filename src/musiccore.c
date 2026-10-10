/* Music core test: the process the system starts for a media app's background audio.
 *
 * Spotify's PS5 app (PPSA05688) is a web app, and its audio engine is a second ELF,
 * app0/eboot2.bin, which param.json names with "musicCoreName": "CustomMusicCore" and
 * "musicCoreTitleId": "NPXS40201". That process is what keeps playing during games. It only
 * uses a small set of libraries:
 *
 *   libSceMusicCoreInterface   sysmodule 0x123: InitializeInterface(argv[1], &{0x10, 0x12}),
 *                              SetFunctionTable(48 callbacks), MainLoop() (blocks)
 *   libSceCustomMusicAudioOut  sysmodule 0x122: Initialize(1024, 1, 1), then Output(4096-byte
 *                              buffers: 1024 frames of 48 kHz S16 stereo)
 *   libSceCustomMusicSysCallWrapper  sysmodule 0x121: threads, sockets, sleep
 *
 * This test copies that shape exactly, with every callback logging when the system calls it,
 * and plays a tone once the system starts the core (callback 1, Spotify's
 * initializeCustomMusicCore). Everything it learns is printed to the kernel log as
 * "ytmcore: ..." and shown as notifications where the core is allowed to post them.
 *
 * No libc: the core links libkernel and the music libraries only, so nothing here depends on
 * what the music core sandbox lets a process load. */

#include <stdint.h>
#include <stddef.h>

int sceSysmoduleLoadModule(uint32_t id);
int sceMusicCoreIfInitializeInterface(const char *arg, const void *param);
int sceMusicCoreIfSetFunctionTable(const void *table);
int sceMusicCoreIfMainLoop(void);
int sceCustomMusicAudioOutInitialize(int grain, int a, int b);
int sceCustomMusicAudioOutOutput(const void *buf);
int sceCustomMusicPthreadAttrInit(void *attr);
int sceCustomMusicPthreadAttrDestroy(void *attr);
int sceCustomMusicPthreadCreate(void *thread, void *attr, void *(*entry)(void *), void *arg,
                                const char *name);
struct ytm_ts {
  int64_t sec;
  int64_t nsec;
};
int sceCustomMusicKernelNanosleep(const struct ytm_ts *req, struct ytm_ts *rem);

long write(int fd, const void *buf, unsigned long n);
void _exit(int status);
typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, unsigned long, int);

#define SYSMODULE_CUSTOM_MUSIC_SYS_CALL_WRAPPER 0x121
#define SYSMODULE_CUSTOM_MUSIC_AUDIO_OUT 0x122
#define SYSMODULE_MUSIC_CORE_INTERFACE 0x123
#define SYSMODULE_SRC_UTILITY 0x103
#define NSLOTS 48
#define GRAIN 1024
#define RATE 48000

/* ---- tiny formatting, no libc ---- */

/* The compiler may still emit calls to these for struct copies and zeroing. */
void *memset(void *p, int c, size_t n) {
  volatile unsigned char *b = p;
  while (n--) *b++ = (unsigned char)c;
  return p;
}
void *memcpy(void *d, const void *s, size_t n) {
  volatile unsigned char *o = d;
  const unsigned char *i = s;
  while (n--) *o++ = *i++;
  return d;
}
#define mset memset

static size_t slen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

typedef struct {
  char buf[512];
  size_t n;
} Line;

static void put(Line *l, const char *s) {
  while (*s && l->n + 1 < sizeof l->buf) l->buf[l->n++] = *s++;
  l->buf[l->n] = 0;
}

static void put_hex(Line *l, uint64_t v) {
  char t[19];
  int i = 18;
  t[i] = 0;
  do {
    t[--i] = "0123456789abcdef"[v & 15];
    v >>= 4;
  } while (v && i > 2);
  t[--i] = 'x';
  t[--i] = '0';
  put(l, t + i);
}

static void put_dec(Line *l, int64_t v) {
  char t[24];
  int i = 23, neg = v < 0;
  uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
  t[i] = 0;
  do {
    t[--i] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  if (neg) t[--i] = '-';
  put(l, t + i);
}

static void emit(Line *l, int toast) {
  Line out = {{0}, 0};
  put(&out, "ytmcore: ");
  put(&out, l->buf);
  put(&out, "\n");
  write(1, out.buf, out.n);
  write(2, out.buf, out.n);
  if (toast) {
    static notify_request_t req;
    mset(&req, 0, sizeof req);
    for (size_t i = 0; i < l->n && i + 1 < sizeof req.message; i++) req.message[i] = l->buf[i];
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  }
}

static void say(const char *what, int64_t value, int toast) {
  Line l = {{0}, 0};
  put(&l, what);
  put(&l, " ");
  put_hex(&l, (uint64_t)value);
  emit(&l, toast);
}

static void sleep_ms(int ms) {
  struct ytm_ts ts = {ms / 1000, (int64_t)(ms % 1000) * 1000000};
  sceCustomMusicKernelNanosleep(&ts, NULL);
}

/* ---- the tone ---- */

static volatile int g_tone_started;
static volatile int g_core_started;
static int16_t g_buf[GRAIN * 2] __attribute__((aligned(64)));

/* A 440 Hz square-ish tone from a small integer sine table, so no libm. */
static const int16_t k_sine[16] = {0,     3444,  6364,  8314,  9000,  8314,  6364,  3444,
                                   0,     -3444, -6364, -8314, -9000, -8314, -6364, -3444};

static void *tone_main(void *arg) {
  int rc;
  uint32_t phase = 0;
  const uint32_t step = (uint32_t)((440ull << 32) / RATE); /* 32.32 cycles per frame */
  (void)arg;
  rc = sceCustomMusicAudioOutInitialize(GRAIN, 1, 1);
  say("AudioOutInitialize ->", rc, 1);
  /* Pulses: 2 s of tone, 1 s of silence, for about two minutes. */
  for (int sec = 0; sec < 120; sec++) {
    int on = (sec % 3) != 2;
    for (int g = 0; g < RATE / GRAIN; g++) {
      for (int i = 0; i < GRAIN; i++) {
        int16_t v = on ? k_sine[phase >> 28] : 0;
        phase += step;
        g_buf[i * 2] = v;
        g_buf[i * 2 + 1] = v;
      }
      rc = sceCustomMusicAudioOutOutput(g_buf);
      if (rc < 0) {
        say("AudioOutOutput failed", rc, 1);
        return NULL;
      }
    }
    if (sec == 0) say("tone playing, AudioOutOutput ->", rc, 1);
  }
  sceCustomMusicAudioOutOutput(NULL);
  say("tone finished", 0, 1);
  return NULL;
}

static void start_tone(const char *why) {
  static uint64_t thread;
  static uint64_t attr[16];
  int rc;
  if (__sync_lock_test_and_set(&g_tone_started, 1)) return;
  say(why, 0, 1);
  sceCustomMusicPthreadAttrInit(attr);
  rc = sceCustomMusicPthreadCreate(&thread, attr, tone_main, NULL, "ytmcore-tone");
  sceCustomMusicPthreadAttrDestroy(attr);
  say("PthreadCreate ->", rc, 1);
}

/* ---- the callback table ---- */

static void log_slot(int slot, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  Line l = {{0}, 0};
  put(&l, "slot ");
  put_dec(&l, slot);
  put(&l, " (");
  put_hex(&l, a);
  put(&l, ", ");
  put_hex(&l, b);
  put(&l, ", ");
  put_hex(&l, c);
  put(&l, ", ");
  put_hex(&l, d);
  put(&l, ")");
  emit(&l, 0);
}

typedef int (*Slot)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*Done)(uint64_t, uint64_t, uint64_t);

/* Spotify's own behaviour per slot, where it is simple enough to copy: what it writes to an
 * out-pointer, and the two slots that call a completion callback straight away. The others
 * just log and succeed. */
static int slot_impl(int s, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  log_slot(s, a, b, c, d);
  switch (s) {
    case 0: /* status: 0x20 bytes, then two 1-based values */
      if (b) {
        mset((void *)b, 0, 0x28);
        ((uint32_t *)b)[8] = 1;
        ((uint32_t *)b)[9] = 1;
      }
      return 0;
    case 1: /* initializeCustomMusicCore */
      g_core_started = 1;
      start_tone("core started by the system (slot 1)");
      return 0;
    case 5: /* (x, y, done, ctx): done(x, 0, ctx) */
      if (c) ((Done)c)(a, 0, d);
      return 0;
    case 6: /* (x, done, ctx): done(x, 0, ctx) */
      if (b) ((Done)b)(a, 0, c);
      return 0;
    case 15:
    case 27: /* a double */
      if (b) *(double *)b = 0.0;
      return 0;
    case 17:
    case 23: /* a bool */
      if (b) *(uint8_t *)b = 0;
      return 0;
    case 19: /* an int */
      if (b) *(uint32_t *)b = 0;
      return 0;
    case 24:
      if (b) *(uint32_t *)b = 0;
      if (c) *(uint32_t *)c = 0;
      return 0;
    case 25:
      if (b) *(uint32_t *)b = 4;
      return 0;
    case 26:
      if (b) *(uint32_t *)b = 2;
      return 0;
    case 29: /* a 0x3020-byte state block */
      if (b) mset((void *)b, 0, 0x3020);
      return 0;
    case 32:
      if (b) *(uint32_t *)b = 50;
      return 0;
    case 41: /* a 0x2530-byte metadata block: 0x155-byte strings */
      if (b) mset((void *)b, 0, 0x2530);
      return 0;
    case 42:
      if (b) mset((void *)b, 0, 24);
      return 0;
    default:
      return 0;
  }
}

#define SLOT(n) \
  static int slot_##n(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return slot_impl(n, a, b, c, d); }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11)
SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(22) SLOT(23) SLOT(24) SLOT(25)
SLOT(26) SLOT(27) SLOT(29) SLOT(32) SLOT(34) SLOT(37) SLOT(38) SLOT(39) SLOT(40) SLOT(41) SLOT(42)

/* The same slots Spotify fills; the ones it leaves NULL stay NULL. */
static Slot g_table[NSLOTS] = {
    [0] = slot_0,   [1] = slot_1,   [2] = slot_2,   [3] = slot_3,   [4] = slot_4,
    [5] = slot_5,   [6] = slot_6,   [7] = slot_7,   [8] = slot_8,   [9] = slot_9,
    [10] = slot_10, [11] = slot_11, [14] = slot_14, [15] = slot_15, [16] = slot_16,
    [17] = slot_17, [18] = slot_18, [19] = slot_19, [22] = slot_22, [23] = slot_23,
    [24] = slot_24, [25] = slot_25, [26] = slot_26, [27] = slot_27, [29] = slot_29,
    [32] = slot_32, [34] = slot_34, [37] = slot_37, [38] = slot_38, [39] = slot_39,
    [40] = slot_40, [41] = slot_41, [42] = slot_42,
};

/* If the system never calls slot 1, try the tone anyway after a while: that alone tells us
 * whether this process's audio is heard. */
static void *fallback_main(void *arg) {
  (void)arg;
  sleep_ms(8000);
  if (!g_core_started) start_tone("slot 1 not called after 8 s, starting tone anyway");
  return NULL;
}

static const uint64_t k_init_param[2] = {0x10, 0x12};

int main(int argc, char **argv) {
  int rc;
  Line l = {{0}, 0};
  static uint64_t fb_thread, fb_attr[16];
  put(&l, "music core started, argc ");
  put_dec(&l, argc);
  if (argc > 1 && argv[1]) {
    put(&l, " argv[1] '");
    put(&l, argv[1]);
    put(&l, "'");
  }
  emit(&l, 1);
  say("load SRC_UTILITY ->", sceSysmoduleLoadModule(SYSMODULE_SRC_UTILITY), 0);
  rc = sceSysmoduleLoadModule(SYSMODULE_MUSIC_CORE_INTERFACE);
  say("load MUSIC_CORE_INTERFACE ->", rc, 0);
  rc = sceSysmoduleLoadModule(SYSMODULE_CUSTOM_MUSIC_SYS_CALL_WRAPPER);
  say("load CUSTOM_MUSIC_SYS_CALL_WRAPPER ->", rc, 0);
  rc = sceSysmoduleLoadModule(SYSMODULE_CUSTOM_MUSIC_AUDIO_OUT);
  say("load CUSTOM_MUSIC_AUDIO_OUT ->", rc, 0);
  rc = sceMusicCoreIfInitializeInterface(argc > 1 ? argv[1] : "", k_init_param);
  say("MusicCoreIfInitializeInterface ->", rc, 1);
  rc = sceMusicCoreIfSetFunctionTable(g_table);
  say("MusicCoreIfSetFunctionTable ->", rc, 1);
  sceCustomMusicPthreadAttrInit(fb_attr);
  sceCustomMusicPthreadCreate(&fb_thread, fb_attr, fallback_main, NULL, "ytmcore-wait");
  sceCustomMusicPthreadAttrDestroy(fb_attr);
  rc = sceMusicCoreIfMainLoop();
  say("MusicCoreIfMainLoop returned", rc, 1);
  (void)slen;
  return 0;
}

/* Startup without libc: the loader hands us the process parameters (argc, then argv). */
__attribute__((visibility("default"), noreturn)) void _start(void *params, void (*teardown)(void)) {
  int argc = *(int *)params;
  char **argv = (char **)((uint8_t *)params + 8);
  (void)teardown;
  _exit(main(argc, argv));
}
