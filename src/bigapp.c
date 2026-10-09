#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys,
                             size_t align);
int sceKernelMapFlexibleMemory(void **addr, size_t len, int prot, int flags);
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

/*
 * libSceLibcInternal's heap is only a few megabytes with the unlimited
 * proc-param size, so the 1080p window surface returns "Out of memory".
 * Back the public allocator with flexible memory, then direct memory.
 * The kernel log shows 448 MiB flexible and 12 GiB direct.
 */
enum { HEAP_MAGIC = 0x59544d48u };

typedef struct HeapBlk {
  uint32_t magic;
  uint32_t free;
  uint64_t size;
  struct HeapBlk *next;
  uint64_t pad[5];
} HeapBlk;

typedef char heap_blk_must_be_64[(sizeof(HeapBlk) == 64) ? 1 : -1];

static HeapBlk *g_heap;
static int g_heap_lock;
static int g_heap_state;
static int g_heap_err;

int ytm_heap_error(void) { return g_heap_err; }

static void heap_lock(void) {
  while (__sync_lock_test_and_set(&g_heap_lock, 1)) {
  }
}

static void heap_unlock(void) { __sync_lock_release(&g_heap_lock); }

static int heap_map(size_t bytes) {
  void *addr = 0;
  int64_t phys = 0;
  size_t pool;
  int rc;
  bytes = (bytes + 0x1ffffful) & ~(size_t)0x1ffffful;
  addr = 0;
  rc = sceKernelMapFlexibleMemory(&addr, bytes, 3, 0);
  if (rc == 0 && addr) {
    g_heap_err = 0;
  } else {
    g_heap_err = rc ? rc : -1;
    pool = sceKernelGetDirectMemorySize();
    if (pool < bytes) pool = bytes;
    rc = sceKernelAllocateDirectMemory(0, (int64_t)pool, bytes, 0x200000ul, 0, &phys);
    if (rc != 0)
      rc = sceKernelAllocateDirectMemory(0, (int64_t)pool, bytes, 0x200000ul, 3, &phys);
    if (rc != 0) {
      g_heap_err = rc;
      return -1;
    }
    addr = 0;
    rc = sceKernelMapDirectMemory(&addr, bytes, 3, 0, phys, 0x200000ul);
    if (rc != 0 || !addr) {
      g_heap_err = rc ? rc : -1;
      return -1;
    }
    g_heap_err = 0;
  }
  g_heap = (HeapBlk *)addr;
  g_heap->magic = HEAP_MAGIC;
  g_heap->free = 1;
  g_heap->size = bytes - sizeof(HeapBlk);
  g_heap->next = 0;
  return 0;
}

static int heap_ready(void) {
  static const size_t sizes[] = {128ul << 20, 64ul << 20, 32ul << 20};
  int i;
  if (g_heap_state == 2) return 0;
  if (g_heap_state == 3) return -1;
  if (!__sync_bool_compare_and_swap(&g_heap_state, 0, 1)) {
    while (g_heap_state == 1) {
    }
    return g_heap_state == 2 ? 0 : -1;
  }
  for (i = 0; i < 3; i++) {
    if (heap_map(sizes[i]) == 0) {
      g_heap_state = 2;
      return 0;
    }
  }
  g_heap_state = 3;
  return -1;
}

static size_t heap_need(size_t n) {
  if (n < 64) n = 64;
  return (n + 63u) & ~(size_t)63u;
}

static void *heap_take(size_t n) {
  HeapBlk *blk;
  n = heap_need(n);
  for (blk = g_heap; blk; blk = blk->next) {
    if (!blk->free || blk->size < n) continue;
    if (blk->size >= n + sizeof(HeapBlk) + 32) {
      HeapBlk *rest = (HeapBlk *)((char *)(blk + 1) + n);
      rest->magic = HEAP_MAGIC;
      rest->free = 1;
      rest->size = blk->size - n - sizeof(HeapBlk);
      rest->next = blk->next;
      blk->next = rest;
      blk->size = n;
    }
    blk->free = 0;
    return blk + 1;
  }
  return 0;
}

void *malloc(size_t n) {
  void *p;
  if (heap_ready() != 0) return 0;
  heap_lock();
  p = heap_take(n);
  heap_unlock();
  return p;
}

void free(void *p) {
  HeapBlk *blk;
  HeapBlk *cur;
  if (!p) return;
  blk = (HeapBlk *)p - 1;
  if (blk->magic != HEAP_MAGIC) return;
  heap_lock();
  blk->free = 1;
  for (cur = g_heap; cur; cur = cur->next) {
    while (cur->free && cur->next && cur->next->free && cur->next->magic == HEAP_MAGIC) {
      cur->size += sizeof(HeapBlk) + cur->next->size;
      cur->next = cur->next->next;
    }
  }
  heap_unlock();
}

void *calloc(size_t n, size_t size) {
  void *p;
  size_t bytes;
  if (n && size > (size_t)-1 / n) return 0;
  bytes = n * size;
  p = malloc(bytes);
  if (p) memset(p, 0, bytes);
  return p;
}

void *realloc(void *p, size_t n) {
  HeapBlk *blk;
  void *nextp;
  size_t old;
  if (!p) return malloc(n);
  if (!n) {
    free(p);
    return 0;
  }
  blk = (HeapBlk *)p - 1;
  if (blk->magic != HEAP_MAGIC) return 0;
  if (blk->size >= n) return p;
  nextp = malloc(n);
  if (!nextp) return 0;
  old = blk->size < n ? blk->size : n;
  memcpy(nextp, p, old);
  free(p);
  return nextp;
}

int posix_memalign(void **out, size_t align, size_t size) {
  void *p;
  if (!out || align < sizeof(void *) || (align & (align - 1)) != 0) return 22;
  /* malloc already returns 64-byte alignment. Larger requests are not used. */
  if (align > 64) return 22;
  p = malloc(size ? size : 1);
  if (!p) return 12;
  *out = p;
  return 0;
}

void *memalign(size_t align, size_t size) {
  void *p = 0;
  if (posix_memalign(&p, align, size) != 0) return 0;
  return p;
}

void *aligned_alloc(size_t align, size_t size) { return memalign(align, size); }

void exit(int status) {
  (void)status;
  sceSystemServiceLoadExec("exit", 0);
  for (;;) sceKernelUsleep(1000000);
}
