#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * SDL asks for scanout pages with sceKernelAllocateMainDirectMemory and a
 * 128 KiB alignment. A BigApp's main-direct pool is often empty (the crash
 * log's mem budget is 0) even though sceKernelGetDirectMemorySize is not.
 * Allocate from that pool at the 2 MiB alignment a title is actually granted.
 * Defining this here keeps the name out of the import table.
 */

unsigned long sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                  size_t align, int type, int64_t *phys);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int sceKernelUsleep(unsigned int micros);

static void remember(int err) {
  if (err >= 0) return;
  if ((err & 0xffff0000) == (int)0x80020000) errno = err & 0xff;
  else if (err > -256) errno = -err;
  else errno = EIO;
}

static int try_one(int64_t end, size_t len, size_t align, int type, int64_t *phys) {
  if (len == 0 || align == 0 || (align & (align - 1)) != 0) return -1;
  if (end < (int64_t)len) return -1;
  return sceKernelAllocateDirectMemory(0, end, len, align, type, phys);
}

int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, int64_t *phys) {
  size_t pool = sceKernelGetDirectMemorySize();
  size_t aligns[2];
  int types[3];
  int64_t ends[5];
  int na = 0;
  int nt = 0;
  int ne = 0;
  int last = -1;
  int i, t, e;
  static const size_t guesses[] = {0x20000000ul, 0x40000000ul, 0x80000000ul, 0x100000000ul};

  aligns[na++] = 0x200000ul;
  if (align >= 0x4000ul && align != 0x200000ul) aligns[na++] = align;
  types[nt++] = type;
  if (type != 3) types[nt++] = 3;
  if (type != 0) types[nt++] = 0;
  if (pool >= len) ends[ne++] = (int64_t)pool;
  for (i = 0; i < 4 && ne < 5; i++) {
    if (guesses[i] < len) continue;
    if (ne == 1 && ends[0] == (int64_t)guesses[i]) continue;
    if (pool >= len && pool <= guesses[i]) continue;
    ends[ne++] = (int64_t)guesses[i];
  }
  if (ne == 0) ends[ne++] = (int64_t)len;

  for (i = 0; i < na; i++) {
    size_t use = (len + aligns[i] - 1) & ~(aligns[i] - 1);
    if (use < len) use = len;
    for (t = 0; t < nt; t++) {
      for (e = 0; e < ne; e++) {
        last = try_one(ends[e], use, aligns[i], types[t], phys);
        if (last == 0) {
          printf("ytmusic: scanout %zu bytes align %zu type %d\n", use, aligns[i], types[t]);
          return 0;
        }
      }
    }
  }
  remember(last);
  printf("ytmusic: scanout alloc failed %d pool %zu\n", last, pool);
  return last == 0 ? -1 : last;
}

void exit(int status) {
  (void)status;
  sceSystemServiceLoadExec("exit", 0);
  for (;;) sceKernelUsleep(1000000);
}
