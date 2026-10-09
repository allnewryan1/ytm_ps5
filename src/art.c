#include "art.h"

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

static int edge_bar(const unsigned char *px, int w, int h, int y) {
  int x, dark = 0, n = 0;
  const unsigned char *row;
  if (y < 0 || y >= h) return 0;
  row = px + (size_t)y * (size_t)w * 4u;
  for (x = 0; x < w; x += 2) {
    int lum = row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
    if (lum < 28) dark++;
    n++;
  }
  return n > 0 && dark * 10 >= n * 9;
}

int art_jpeg_square(const unsigned char *jpg, int n, unsigned char *dst, int side) {
  int w = 0, h = 0, comp = 0, crop, x0, y0, y, x, top, bot, left, right, iw, ih;
  unsigned char *px;
  if (!jpg || n < 4 || !dst || side < 1) return -1;
  if (jpg[0] != 0xff || jpg[1] != 0xd8) return -1;
  px = stbi_load_from_memory(jpg, n, &w, &h, &comp, 4);
  if (!px || w < 1 || h < 1) {
    if (px) stbi_image_free(px);
    return -1;
  }
  /* YouTube's 4:3 thumbnails letterbox a 16:9 frame. Drop those black bands. */
  top = 0;
  bot = h;
  while (top < h / 3 && edge_bar(px, w, h, top)) top++;
  while (bot > top + h / 2 && edge_bar(px, w, h, bot - 1)) bot--;
  if (top < 8 && h - bot < 8) {
    top = 0;
    bot = h;
  }
  left = 0;
  right = w;
  iw = right - left;
  ih = bot - top;
  if (iw < 8 || ih < 8) {
    top = 0;
    bot = h;
    left = 0;
    right = w;
    iw = w;
    ih = h;
  }
  crop = iw < ih ? iw : ih;
  x0 = left + (iw - crop) / 2;
  y0 = top + (ih - crop) / 2;
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
