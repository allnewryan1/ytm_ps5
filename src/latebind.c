#include <stdio.h>

/*
 * A BigApp does not map libSceKeyboard or libSceImeDialog, so those GOT
 * slots stay 0 and SDL_Init dies on the first call. Bind them through
 * libkernel once the sprx is actually loaded.
 */

int sceKernelLoadStartModule(const char *name, unsigned long argc, const void *argv,
                             unsigned int flags, const void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **addr);

static int g_kb;
static int g_ime;
static int (*p_kb_init)(void);
static int (*p_kb_open)(int, int, int, void *);
static int (*p_kb_read)(int, void *);
static int (*p_kb_close)(int);
static int (*p_ime_init)(const void *, void *);
static int (*p_ime_result)(void *);
static int (*p_ime_term)(void);
static int (*p_ime_status)(void);

static int load_one(const char *file) {
  int res = 0;
  int handle = sceKernelLoadStartModule(file, 0, 0, 0, 0, &res);
  if (handle > 0) return handle;
  if (res > 0) return res;
  return -1;
}

static int load_named(const char *soname) {
  char full[96];
  int handle;
  snprintf(full, sizeof full, "/system/common/lib/%s", soname);
  handle = load_one(full);
  if (handle > 0) return handle;
  return load_one(soname);
}

static void bind(int handle, const char *name, const char *nid, void **out) {
  void *addr = 0;
  if (handle > 0 && (sceKernelDlsym(handle, name, &addr) != 0 || !addr)) {
    addr = 0;
    if (sceKernelDlsym(handle, nid, &addr) != 0) addr = 0;
  }
  *out = addr;
}

static void load_keyboard(void) {
  if (g_kb != 0) return;
  g_kb = load_named("libSceKeyboard.sprx");
  if (g_kb < 0) g_kb = -2;
  printf("ytmusic: libSceKeyboard %d\n", g_kb);
  bind(g_kb, "sceKeyboardInit", "wadT3QBCGY0", (void **)&p_kb_init);
  bind(g_kb, "sceKeyboardOpen", "HJ+KnEHcaxI", (void **)&p_kb_open);
  bind(g_kb, "sceKeyboardReadState", "6HpE68bzX6M", (void **)&p_kb_read);
  bind(g_kb, "sceKeyboardClose", "0LWei+c7RNc", (void **)&p_kb_close);
}

static void load_ime(void) {
  if (g_ime != 0) return;
  g_ime = load_named("libSceImeDialog.sprx");
  if (g_ime < 0) g_ime = -2;
  printf("ytmusic: libSceImeDialog %d\n", g_ime);
  bind(g_ime, "sceImeDialogInit", "NUeBrN7hzf0", (void **)&p_ime_init);
  bind(g_ime, "sceImeDialogGetResult", "x01jxu+vxlc", (void **)&p_ime_result);
  bind(g_ime, "sceImeDialogTerm", "gyTyVn+bXMw", (void **)&p_ime_term);
  bind(g_ime, "sceImeDialogGetStatus", "IADmD4tScBY", (void **)&p_ime_status);
}

int sceKeyboardInit(void) {
  load_keyboard();
  if (!p_kb_init) return -1;
  return p_kb_init();
}

int sceKeyboardOpen(int user, int a, int b, void *extra) {
  load_keyboard();
  if (!p_kb_open) return -1;
  return p_kb_open(user, a, b, extra);
}

int sceKeyboardReadState(int handle, void *state) {
  load_keyboard();
  if (!p_kb_read) return -1;
  return p_kb_read(handle, state);
}

int sceKeyboardClose(int handle) {
  load_keyboard();
  if (!p_kb_close) return -1;
  return p_kb_close(handle);
}

int sceImeDialogInit(const void *param, void *extra) {
  load_ime();
  if (!p_ime_init) return -1;
  return p_ime_init(param, extra);
}

int sceImeDialogGetResult(void *result) {
  load_ime();
  if (!p_ime_result) return -1;
  return p_ime_result(result);
}

int sceImeDialogTerm(void) {
  load_ime();
  if (!p_ime_term) return -1;
  return p_ime_term();
}

int sceImeDialogGetStatus(void) {
  load_ime();
  if (!p_ime_status) return 0;
  return p_ime_status();
}
