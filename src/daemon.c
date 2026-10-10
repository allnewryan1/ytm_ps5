/* ytmusicd: the background payload the app starts through the ELF loader.
 *
 * It lives in its own process, so it keeps running when the app is suspended because a game
 * took focus. Today it only proves the lifecycle: the app deploys it, connects, and it exits
 * with the app. The playback engine moves in here once background audio is confirmed.
 *
 * It exits when
 *   - the app sends QUIT (the Quit option),
 *   - the app's connection closes (the kernel closes it when the app is force closed or crashes),
 *   - the app's process no longer exists (a backstop if the close is never delivered),
 *   - no app says HELLO within ORPHAN_SECS of launch (deployed, but the app died first).
 * It never exits for silence: a suspended app sends nothing but must keep its music. */

#include "ytmd.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#ifdef __PROSPERO__
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
/* struct kinfo_proc on the PS5: ki_pid and ki_tdname (the representative thread's name). */
#define KINFO_PID_OFFSET 72
#define KINFO_TDNAME_OFFSET 447
#endif

#define LOG_DIR "/data/ytmusic"
#define LOG_PATH LOG_DIR "/daemon.log"
#define LOG_PREV LOG_DIR "/daemon.prev.log"
#define ORPHAN_SECS 15
#define HELLO_SECS 5
#define PID_CHECK_MS 2000
#define EXIT_WATCHDOG_SECS 5

/* Every thread names itself this, first thing (YTMD_THREAD_NAME): the loader calls every raw
 * payload "payload.elf", so the name is the only way to tell a stray ytmusicd from kstuff,
 * ShadowMountPlus or any other payload. The name is per thread, not inherited. */
static void name_thread(const char *name) {
#ifdef SYS_thr_set_name
  syscall(SYS_thr_set_name, -1, name);
#else
  (void)name;
#endif
}

static void logf_(const char *fmt, ...) {
  char line[512];
  struct timespec ts;
  va_list ap;
  clock_gettime(CLOCK_REALTIME, &ts);
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  fprintf(stderr, "[%lld] ytmusicd: %s\n", (long long)ts.tv_sec, line);
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The loader hands us its client socket as stdin, stdout and stderr. That socket closes as
 * soon as the app has sent us, and a write to it would then raise SIGPIPE and kill us. */
static void detach_stdio(void) {
  int nul = open("/dev/null", O_RDWR);
  int lf;
  mkdir(LOG_DIR, 0755);
  /* The previous run's log is kept: after a crash and a restart, its last line says why. */
  rename(LOG_PATH, LOG_PREV);
  lf = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (lf < 0) lf = nul;
  if (nul >= 0) dup2(nul, STDIN_FILENO);
  if (lf >= 0) {
    dup2(lf, STDOUT_FILENO);
    dup2(lf, STDERR_FILENO);
  }
  if (lf > STDERR_FILENO) close(lf);
  if (nul > STDERR_FILENO && nul != lf) close(nul);
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
  signal(SIGPIPE, SIG_IGN);
}

/* A crash must end the process at once. Left to the default action, a fault on firmware 13.60
 * goes to a core dump that can wedge the process for good: it ignores SIGKILL and keeps the port
 * half open, so no new daemon can start until a reboot (ps5upload saw this; their #417).
 * Only async-signal-safe calls in here. The kernel closes our sockets on _exit. */
static void on_fatal(int sig) {
  char msg[] = "ytmusicd: fatal signal __, exiting\n";
  msg[23] = (char)('0' + (sig / 10) % 10);
  msg[24] = (char)('0' + sig % 10);
  ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1);
  (void)w;
  _exit(128 + sig);
}

static void trap_fatal_signals(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS};
  for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], on_fatal);
}


/* SIGKILL every other ytmusicd and wait (bounded) until each is gone. The app only deploys a new
 * daemon after the one on the port failed to answer or was from another build, so anything
 * still holding the port is stale or wedged. Returns how many were ended. */
#ifdef __PROSPERO__
/* Whether a thread name is one of ours: "ytmusicd", or "ytmusicd-<role>" for worker threads.
 * Exact, so nothing that merely starts with the same letters is ever killed. */
static int name_is_ours(const char *n) {
  size_t k = strlen(YTMD_THREAD_NAME);
  return strncmp(n, YTMD_THREAD_NAME, k) == 0 && (n[k] == 0 || n[k] == '-');
}
#endif

static int sweep_others(void) {
  int ended = 0;
#ifdef __PROSPERO__
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t need = 0, got;
  unsigned char *buf;
  pid_t victims[16];
  int nv = 0;
  pid_t me = getpid();
  if (sysctl(mib, 4, NULL, &need, NULL, 0) != 0 || need == 0) return 0;
  got = need + need / 4 + 1024;
  buf = malloc(got);
  if (!buf) return 0;
  if (sysctl(mib, 4, buf, &got, NULL, 0) != 0) {
    free(buf);
    return 0;
  }
  for (size_t at = 0; at + sizeof(int) <= got;) {
    int sz = *(int *)(buf + at);
    char name[24];
    pid_t pid;
    size_t i;
    if (sz <= KINFO_TDNAME_OFFSET || at + (size_t)sz > got) break;
    pid = *(pid_t *)(buf + at + KINFO_PID_OFFSET);
    for (i = 0; i + 1 < sizeof name && KINFO_TDNAME_OFFSET + i < (size_t)sz &&
                buf[at + KINFO_TDNAME_OFFSET + i];
         i++)
      name[i] = (char)buf[at + KINFO_TDNAME_OFFSET + i];
    name[i] = 0;
    at += (size_t)sz;
    if (pid <= 1 || pid == me || !name_is_ours(name) || nv >= 16) continue;
    if (kill(pid, SIGKILL) == 0) {
      logf_("ended stale ytmusicd pid %d", (int)pid);
      victims[nv++] = pid;
    }
  }
  free(buf);
  /* kill() returning 0 means delivered, not dead. Confirm each, for up to a second. */
  for (int tries = 0; tries < 20 && ended < nv; tries++) {
    ended = 0;
    for (int v = 0; v < nv; v++)
      if (kill(victims[v], 0) != 0 && errno == ESRCH) ended++;
    if (ended < nv) usleep(50000);
  }
  if (ended < nv) logf_("%d stale ytmusicd did not exit", nv - ended);
#endif
  return ended;
}

static void quiet_socket(int fd) {
  int one = 1;
  struct timeval tv = {2, 0};
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  /* A half-sent request must not wedge the daemon. */
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  (void)one;
}

static int listen_local(void) {
  struct sockaddr_in sa;
  int one = 1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(YTMD_PORT);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

typedef struct {
  int fd;
  int hello;          /* this client said HELLO */
  pid_t pid;          /* its process */
  long long deadline; /* say HELLO by then */
} Client;

enum { KEEP = 0, DROP = 1, EXIT = 2 };

/* One request. KEEP serves on, DROP closes this client, EXIT ends the daemon. */
static int serve_one(Client *c) {
  YtmdHdr h;
  union {
    YtmdHello hello;
    char raw[YTMD_BODY_MAX];
  } body;
  if (ytmd_recv_msg(c->fd, &h, &body, sizeof body) != 0) return c->hello ? EXIT : DROP;
  switch (h.op) {
    case YTMD_HELLO: {
      YtmdHello me;
      if (h.len < sizeof(YtmdHello)) return DROP;
      c->hello = 1;
      c->pid = body.hello.pid;
      me.version = YTMD_VERSION;
      me.pid = getpid();
      logf_("app pid %d connected (protocol %u)", (int)c->pid, body.hello.version);
      return ytmd_send_msg(c->fd, h.op, 0, &me, sizeof me) == 0 ? KEEP : EXIT;
    }
    case YTMD_PING:
      return ytmd_send_msg(c->fd, h.op, 0, NULL, 0) == 0 ? KEEP : EXIT;
    case YTMD_QUIT:
      logf_("quit requested");
      ytmd_send_msg(c->fd, h.op, 0, NULL, 0);
      return EXIT;
    default:
      return ytmd_send_msg(c->fd, h.op, -ENOSYS, NULL, 0) == 0 ? KEEP : EXIT;
  }
}

int main(void) {
  Client c = {-1, 0, 0, 0};
  long long orphan_at;
  long long next_pid_check = 0;
  int lfd;

  name_thread(YTMD_THREAD_NAME);
  detach_stdio();
  trap_fatal_signals();
  lfd = listen_local();
  if (lfd < 0) {
    /* Bind first, before any other work, so the app's probe finds us as soon as possible.
     * A port that is still held belongs to a stale or wedged ytmusicd: end it and retry. */
    logf_("port %d busy (%s); taking over", YTMD_PORT, strerror(errno));
    sweep_others();
    for (int i = 0; i < 30 && lfd < 0; i++) {
      usleep(100000);
      lfd = listen_local();
    }
    if (lfd < 0) {
      logf_("port %d still busy; exiting", YTMD_PORT);
      return 0;
    }
  }
  logf_("pid %d listening on 127.0.0.1:%d", (int)getpid(), YTMD_PORT);
  orphan_at = now_ms() + ORPHAN_SECS * 1000;

  for (;;) {
    struct pollfd pf[2];
    int n = 0;
    long long t;
    pf[n].fd = lfd;
    pf[n].events = POLLIN;
    pf[n++].revents = 0;
    if (c.fd >= 0) {
      pf[n].fd = c.fd;
      pf[n].events = POLLIN;
      pf[n++].revents = 0;
    }
    if (poll(pf, (nfds_t)n, 500) < 0 && errno != EINTR) {
      logf_("poll: %s", strerror(errno));
      break;
    }
    t = now_ms();

    if (pf[0].revents & POLLIN) {
      int nfd = accept(lfd, NULL, NULL);
      if (nfd >= 0) {
        quiet_socket(nfd);
        /* The newest app wins. An older connection is a stale or hung app. */
        if (c.fd >= 0) {
          logf_("new app connection replaces pid %d", (int)c.pid);
          close(c.fd);
        }
        c.fd = nfd;
        c.hello = 0;
        c.pid = 0;
        c.deadline = t + HELLO_SECS * 1000;
      }
    }

    if (c.fd >= 0 && n > 1 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      int r = serve_one(&c);
      if (r == EXIT) {
        if (c.hello) logf_("app pid %d is gone or quit; exiting", (int)c.pid);
        break;
      }
      if (r == DROP) {
        close(c.fd);
        c.fd = -1;
      }
    }

    if (c.fd >= 0 && !c.hello && t > c.deadline) {
      close(c.fd);
      c.fd = -1;
    }
    if (c.fd < 0 && t > orphan_at) {
      logf_("no app connected within %d s; exiting", ORPHAN_SECS);
      break;
    }
    if (c.fd >= 0 && c.hello && c.pid > 0 && t >= next_pid_check) {
      next_pid_check = t + PID_CHECK_MS;
      if (kill(c.pid, 0) != 0 && errno == ESRCH) {
        logf_("app pid %d no longer exists; exiting", (int)c.pid);
        break;
      }
    }
  }

  /* Teardown is bounded: if anything hangs (the engine's threads, later), SIGALRM's default
   * action ends the process. */
  alarm(EXIT_WATCHDOG_SECS);
  if (c.fd >= 0) close(c.fd);
  close(lfd);
  logf_("stopped");
  return 0;
}
