#ifndef YTM_YTMD_H
#define YTM_YTMD_H

/* Protocol between the app and ytmusicd, the background payload.
 *
 * The app sends the daemon ELF to the ELF loader (127.0.0.1:9021), connects to the daemon on
 * 127.0.0.1:YTMD_PORT and says HELLO with its pid. The daemon serves one app at a time and
 * exits when that app sends QUIT, closes the connection (the kernel closes it when the app
 * is force closed), or no longer exists. A suspended app keeps its connection, so silence is
 * never a reason to exit: music must keep playing while a game has focus.
 *
 * Every message is a YtmdHdr and len bytes of body. Every request gets one reply whose rc is
 * 0 or a negative error. Both ends are the same build, so structs go over the wire as is. */

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define YTMD_PORT 47330
#define YTMD_LOADER_PORT 9021
#define YTMD_MAGIC 0x444d5459u /* "YTMD" */
#define YTMD_VERSION 1
#define YTMD_BODY_MAX 16384

enum {
  YTMD_HELLO = 1, /* YtmdHello -> YtmdHello */
  YTMD_PING = 2,  /* -> nothing */
  YTMD_QUIT = 3,  /* -> nothing, then the daemon exits */
  /* The playback engine's commands (start, pause, seek, volume, status, queue) go here. */
};

typedef struct {
  uint32_t magic;
  uint16_t op;
  uint16_t len;
  int32_t rc; /* replies only; 0 in requests */
} YtmdHdr;

typedef struct {
  uint32_t version;
  int32_t pid;
} YtmdHello;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Send or receive exactly n bytes. 0 on success, -1 on error or end of stream. */
static inline int ytmd_send_all(int fd, const void *buf, size_t n) {
  const char *p = (const char *)buf;
  while (n > 0) {
    ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

static inline int ytmd_recv_all(int fd, void *buf, size_t n) {
  char *p = (char *)buf;
  while (n > 0) {
    ssize_t k = recv(fd, p, n, 0);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

static inline int ytmd_send_msg(int fd, uint16_t op, int32_t rc, const void *body, uint16_t len) {
  YtmdHdr h;
  h.magic = YTMD_MAGIC;
  h.op = op;
  h.len = len;
  h.rc = rc;
  if (ytmd_send_all(fd, &h, sizeof h) != 0) return -1;
  return len ? ytmd_send_all(fd, body, len) : 0;
}

/* Read one message. Body bytes beyond cap are discarded. 0 on success, -1 on error. */
static inline int ytmd_recv_msg(int fd, YtmdHdr *h, void *body, uint16_t cap) {
  uint16_t keep;
  if (ytmd_recv_all(fd, h, sizeof *h) != 0 || h->magic != YTMD_MAGIC || h->len > YTMD_BODY_MAX)
    return -1;
  keep = h->len < cap ? h->len : cap;
  if (keep && ytmd_recv_all(fd, body, keep) != 0) return -1;
  for (uint16_t left = (uint16_t)(h->len - keep); left > 0;) {
    char sink[256];
    uint16_t k = left < sizeof sink ? left : (uint16_t)sizeof sink;
    if (ytmd_recv_all(fd, sink, k) != 0) return -1;
    left = (uint16_t)(left - k);
  }
  return 0;
}

#endif
