/* App side of ytmusicd: deploy it, keep the connection, stop it on Quit.
 *
 * The daemon ELF is embedded in the app (daemon_blob.S). ytmd_start() sends it to the ELF
 * loader on 127.0.0.1:9021, which runs it in its own process, then connects and says HELLO.
 * The connection is what ties the two together: when the app is force closed the kernel
 * closes it and the daemon exits. ytmd_stop() is the clean path for the Quit option. */

#include "daemon_client.h"
#include "ytmd.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

extern const unsigned char ytm_daemon_elf[];
extern const unsigned char ytm_daemon_elf_end[];

#define PING_MS 2000
#define DEPLOY_WAIT_MS 6000
#define MAX_RESTARTS 3

static int g_fd = -1;
static int g_pid;
static int g_restarts;
static long long g_next_ping;
static char g_status[160] = "Not started";

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* Connect to 127.0.0.1:port within timeout_ms. A blocking socket with 1 s I/O timeouts. */
static int connect_local(int port, int timeout_ms) {
  struct sockaddr_in sa;
  struct pollfd pf;
  struct timeval tv = {1, 0};
  int one = 1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int fl, err = 0;
  socklen_t elen = sizeof err;
  if (fd < 0) return -1;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    if (errno != EINPROGRESS) goto fail;
    pf.fd = fd;
    pf.events = POLLOUT;
    pf.revents = 0;
    if (poll(&pf, 1, timeout_ms) != 1) goto fail;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) goto fail;
  }
  fcntl(fd, F_SETFL, fl);
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  (void)one;
  return fd;
fail:
  close(fd);
  return -1;
}

/* One request and its reply. Returns the reply rc, or -1 when the connection failed. */
static int call(int fd, uint16_t op, const void *body, uint16_t len, void *reply, uint16_t cap) {
  YtmdHdr h;
  if (fd < 0 || ytmd_send_msg(fd, op, 0, body, len) != 0) return -1;
  if (ytmd_recv_msg(fd, &h, reply, cap) != 0 || h.op != op) return -1;
  return h.rc;
}

/* HELLO on fd. The daemon's pid, or -1 if it is not a daemon of this version. */
static int hello(int fd) {
  YtmdHello me, them;
  me.version = YTMD_VERSION;
  me.pid = getpid();
  memset(&them, 0, sizeof them);
  if (call(fd, YTMD_HELLO, &me, sizeof me, &them, sizeof them) != 0) return -1;
  if (them.version != YTMD_VERSION || them.pid <= 0) return -1;
  return them.pid;
}

static void drop(void) {
  if (g_fd >= 0) close(g_fd);
  g_fd = -1;
  g_pid = 0;
}

static int adopt(int fd) {
  int pid = hello(fd);
  if (pid <= 0) return -1;
  g_fd = fd;
  g_pid = pid;
  g_next_ping = now_ms() + PING_MS;
  snprintf(g_status, sizeof g_status, "Running (pid %d)", pid);
  return 0;
}

/* Ask whatever owns the daemon port to quit, and wait until the port is free. */
static void retire(int fd) {
  call(fd, YTMD_QUIT, NULL, 0, NULL, 0);
  close(fd);
  for (int i = 0; i < 20; i++) {
    int probe = connect_local(YTMD_PORT, 100);
    if (probe < 0) return;
    close(probe);
    nap_ms(100);
  }
}

static int deploy(void) {
  size_t n = (size_t)(ytm_daemon_elf_end - ytm_daemon_elf);
  int fd = connect_local(YTMD_LOADER_PORT, 1500);
  if (fd < 0) {
    snprintf(g_status, sizeof g_status, "No ELF loader on port %d", YTMD_LOADER_PORT);
    return -1;
  }
  /* The loader reads exactly one ELF, sized from its headers, then starts it. */
  if (n < 64 || ytmd_send_all(fd, ytm_daemon_elf, n) != 0) {
    close(fd);
    snprintf(g_status, sizeof g_status, "Could not send the background player");
    return -1;
  }
  close(fd);
  return 0;
}

int ytmd_start(void) {
  int fd;
  long long until;
  if (g_fd >= 0) return 0;
  fd = connect_local(YTMD_PORT, 300);
  if (fd >= 0) {
    /* A daemon from this build is reused. Anything else on the port is told to quit. */
    if (adopt(fd) == 0) return 0;
    retire(fd);
  }
  if (deploy() != 0) return -1;
  until = now_ms() + DEPLOY_WAIT_MS;
  while (now_ms() < until) {
    nap_ms(100);
    fd = connect_local(YTMD_PORT, 200);
    if (fd < 0) continue;
    if (adopt(fd) == 0) return 0;
    close(fd);
  }
  snprintf(g_status, sizeof g_status, "Background player did not start");
  return -1;
}

void ytmd_tick(void) {
  long long t;
  if (g_fd < 0) return;
  t = now_ms();
  if (t < g_next_ping) return;
  g_next_ping = t + PING_MS;
  if (call(g_fd, YTMD_PING, NULL, 0, NULL, 0) == 0) return;
  drop();
  if (g_restarts >= MAX_RESTARTS) {
    snprintf(g_status, sizeof g_status, "Background player stopped");
    return;
  }
  g_restarts++;
  if (ytmd_start() != 0)
    snprintf(g_status, sizeof g_status, "Background player stopped and did not restart");
}

void ytmd_stop(void) {
  if (g_fd < 0) return;
  call(g_fd, YTMD_QUIT, NULL, 0, NULL, 0);
  drop();
  snprintf(g_status, sizeof g_status, "Stopped");
}

int ytmd_running(void) { return g_fd >= 0; }
int ytmd_pid(void) { return g_pid; }
const char *ytmd_status(void) { return g_status; }
