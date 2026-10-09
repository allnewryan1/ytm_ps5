#include "art.h"

#include <stdlib.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* A row or column of letterbox bar: every sampled pixel is near black. */
static int dark_line(const unsigned char *px, int w, int h, int pos, int horiz) {
  int len = horiz ? w : h;
  int step = len / 48 > 0 ? len / 48 : 1;
  int i;
  for (i = step / 2; i < len; i += step) {
    const unsigned char *p = horiz ? px + ((size_t)pos * (size_t)w + (size_t)i) * 4u
                                   : px + ((size_t)i * (size_t)w + (size_t)pos) * 4u;
    if (p[0] > 28 || p[1] > 28 || p[2] > 28) return 0;
  }
  return 1;
}

/* YouTube stills put 16:9 video in a 4:3 frame (sddefault, hqdefault) with black bars, and
 * square art in a 16:9 frame with bars on the sides. Bars are symmetric, so only an equal
 * pair is trimmed. A dark cover that is already square is left alone. */
static void trim_bars(const unsigned char *px, int w, int h, int *x0, int *y0, int *cw, int *ch) {
  int top = 0, bot = 0, lft = 0, rgt = 0;
  *x0 = 0;
  *y0 = 0;
  *cw = w;
  *ch = h;
  if (w < 16 || h < 16) return;
  if (w * 50 > h * 49 && w * 49 < h * 50) return;
  while (top < h * 3 / 10 && dark_line(px, w, h, top, 1)) top++;
  while (bot < h * 3 / 10 && dark_line(px, w, h, h - 1 - bot, 1)) bot++;
  while (lft < w * 3 / 10 && dark_line(px, w, h, lft, 0)) lft++;
  while (rgt < w * 3 / 10 && dark_line(px, w, h, w - 1 - rgt, 0)) rgt++;
  if (top >= 4 && bot >= 4 && abs(top - bot) <= 4 + h / 40) {
    *y0 = top;
    *ch = h - top - bot;
  }
  if (lft >= 4 && rgt >= 4 && abs(lft - rgt) <= 4 + w / 40) {
    *x0 = lft;
    *cw = w - lft - rgt;
  }
  if (*cw < 8 || *ch < 8) {
    *x0 = 0;
    *y0 = 0;
    *cw = w;
    *ch = h;
  }
}

int art_jpeg_square(const unsigned char *jpg, int n, unsigned char *dst, int side) {
  int w = 0, h = 0, comp = 0, crop, x0, y0, y, x;
  int bx = 0, by = 0, bw = 0, bh = 0;
  unsigned char *px;
  if (!jpg || n < 4 || !dst || side < 1) return -1;
  if (jpg[0] != 0xff || jpg[1] != 0xd8) return -1;
  px = stbi_load_from_memory(jpg, n, &w, &h, &comp, 4);
  if (!px || w < 1 || h < 1) {
    if (px) stbi_image_free(px);
    return -1;
  }
  trim_bars(px, w, h, &bx, &by, &bw, &bh);
  crop = bw < bh ? bw : bh;
  x0 = bx + (bw - crop) / 2;
  y0 = by + (bh - crop) / 2;
  for (y = 0; y < side; y++) {
    int sy = y0 + (y * crop) / side;
    if (sy >= h) sy = h - 1;
    for (x = 0; x < side; x++) {
      int sx = x0 + (x * crop) / side;
      unsigned char *d;
      const unsigned char *s;
      if (sx >= w) sx = w - 1;
      s = px + ((size_t)sy * (size_t)w + (size_t)sx) * 4u;
      d = dst + ((size_t)y * (size_t)side + (size_t)x) * 4u;
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = 255;
    }
  }
  stbi_image_free(px);
  return 0;
}
