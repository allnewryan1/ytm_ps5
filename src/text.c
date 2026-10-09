#include "text.h"

#include "ui_font.h"

#include <string.h>

static void clip_rect(const Draw *d, int *x, int *y, int *w, int *h) {
  int sw, sh, x1, y1;
  if (!d->surf) {
    *w = 0;
    *h = 0;
    return;
  }
  sw = d->surf->w;
  sh = d->surf->h;
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  x1 = *x + *w;
  y1 = *y + *h;
  if (x1 > sw) *w = sw - *x;
  if (y1 > sh) *h = sh - *y;
  if (*w < 0) *w = 0;
  if (*h < 0) *h = 0;
}

static void put_px(Draw *d, int x, int y, int r, int g, int b, int a) {
  Uint8 *p;
  Uint32 pix, np;
  int dr, dg, db, inv;
  if (a < 12 || !d->surf || !d->surf->pixels || d->bpp != 4) return;
  if (x < 0 || y < 0 || x >= d->surf->w || y >= d->surf->h) return;
  p = (Uint8 *)d->surf->pixels + y * d->surf->pitch + x * 4;
  if (a >= 250) {
    np = ((unsigned)r << d->rshift) | ((unsigned)g << d->gshift) | ((unsigned)b << d->bshift);
    if (d->amask) np |= d->amask;
    *(Uint32 *)p = np;
    return;
  }
  pix = *(Uint32 *)p;
  inv = 255 - a;
  dr = (int)((pix >> d->rshift) & 0xffu);
  dg = (int)((pix >> d->gshift) & 0xffu);
  db = (int)((pix >> d->bshift) & 0xffu);
  dr = (dr * inv + r * a) >> 8;
  dg = (dg * inv + g * a) >> 8;
  db = (db * inv + b * a) >> 8;
  np = ((unsigned)dr << d->rshift) | ((unsigned)dg << d->gshift) | ((unsigned)db << d->bshift);
  if (d->amask) np |= d->amask;
  *(Uint32 *)p = np;
}

void draw_begin(Draw *d, SDL_Surface *surf) {
  int w = surf ? surf->w : 1920;
  int h = surf ? surf->h : 1080;
  float sx, sy;
  memset(d, 0, sizeof *d);
  if (w < 16) w = 1920;
  if (h < 16) h = 1080;
  sx = (float)w / 1920.0f;
  sy = (float)h / 1080.0f;
  d->surf = surf;
  d->s = sx < sy ? sx : sy;
  d->ox = (int)((w - 1920.0f * d->s) * 0.5f);
  d->oy = (int)((h - 1080.0f * d->s) * 0.5f);
  if (!surf || !surf->format || surf->format->BytesPerPixel != 4) return;
  d->bpp = 4;
  d->rshift = surf->format->Rshift;
  d->gshift = surf->format->Gshift;
  d->bshift = surf->format->Bshift;
  d->amask = surf->format->Amask;
  if (SDL_MUSTLOCK(surf)) {
    if (SDL_LockSurface(surf) != 0) return;
    d->locked = 1;
  } else {
    d->locked = 0;
  }
}

void draw_end(Draw *d) {
  if (!d || !d->surf) return;
  if (d->locked) SDL_UnlockSurface(d->surf);
  d->locked = 0;
}

void fill_v(Draw *d, int x, int y, int w, int h, int r, int g, int b) {
  int x0, y0, pw, ph;
  if (!d->surf || w < 1 || h < 1) return;
  x0 = d->ox + (int)(x * d->s);
  y0 = d->oy + (int)(y * d->s);
  pw = (int)(w * d->s);
  ph = (int)(h * d->s);
  if (pw < 1) pw = 1;
  if (ph < 1) ph = 1;
  if (d->bpp != 4 || !d->surf->pixels) {
    SDL_Rect rc;
    rc.x = x0;
    rc.y = y0;
    rc.w = pw;
    rc.h = ph;
    SDL_FillRect(d->surf, &rc, SDL_MapRGB(d->surf->format, (Uint8)r, (Uint8)g, (Uint8)b));
    return;
  }
  clip_rect(d, &x0, &y0, &pw, &ph);
  for (int row = 0; row < ph; row++) {
    Uint8 *p = (Uint8 *)d->surf->pixels + (y0 + row) * d->surf->pitch + x0 * 4;
    Uint32 np = ((unsigned)r << d->rshift) | ((unsigned)g << d->gshift) | ((unsigned)b << d->bshift);
    if (d->amask) np |= d->amask;
    for (int col = 0; col < pw; col++) ((Uint32 *)p)[col] = np;
  }
}

static int round_inset(int dist, int rad) {
  int dy, lo, hi;
  if (dist >= rad) return 0;
  dy = rad - dist;
  lo = 0;
  hi = rad;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (mid * mid + dy * dy <= rad * rad) lo = mid;
    else hi = mid - 1;
  }
  return rad - lo;
}

void fill_round(Draw *d, int x, int y, int w, int h, int rad, int r, int g, int b) {
  int x0, y0, pw, ph, prad;
  Uint32 np;
  if (rad < 1) {
    fill_v(d, x, y, w, h, r, g, b);
    return;
  }
  if (!d->surf || w < 1 || h < 1) return;
  x0 = d->ox + (int)(x * d->s);
  y0 = d->oy + (int)(y * d->s);
  pw = (int)(w * d->s);
  ph = (int)(h * d->s);
  if (pw < 1) pw = 1;
  if (ph < 1) ph = 1;
  prad = (int)(rad * d->s);
  if (prad < 1) prad = 1;
  if (prad > pw / 2) prad = pw / 2;
  if (prad > ph / 2) prad = ph / 2;
  if (d->bpp != 4 || !d->surf->pixels) {
    fill_v(d, x, y, w, h, r, g, b);
    return;
  }
  np = ((unsigned)r << d->rshift) | ((unsigned)g << d->gshift) | ((unsigned)b << d->bshift);
  if (d->amask) np |= d->amask;
  for (int row = 0; row < ph; row++) {
    int inset = 0;
    int sx, sw;
    if (row < prad) inset = round_inset(row, prad);
    else if (row >= ph - prad) inset = round_inset(ph - 1 - row, prad);
    sx = x0 + inset;
    sw = pw - inset * 2;
    if (sw < 1) continue;
    {
      int sy = y0 + row;
      int clip_x = sx, clip_y = sy, clip_w = sw, clip_h = 1;
      clip_rect(d, &clip_x, &clip_y, &clip_w, &clip_h);
      if (clip_h < 1 || clip_w < 1) continue;
      {
        Uint8 *p = (Uint8 *)d->surf->pixels + clip_y * d->surf->pitch + clip_x * 4;
        for (int col = 0; col < clip_w; col++) ((Uint32 *)p)[col] = np;
      }
    }
  }
}

static unsigned utf8_next(const unsigned char **p) {
  const unsigned char *s = *p;
  unsigned cp;
  if (s[0] < 0x80) {
    *p = s + 1;
    return s[0];
  }
  if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    cp = ((unsigned)(s[0] & 0x1F) << 6) | (unsigned)(s[1] & 0x3F);
    *p = s + 2;
    return cp < 0x80 ? (unsigned)'?' : cp;
  }
  if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    cp = ((unsigned)(s[0] & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (unsigned)(s[2] & 0x3F);
    *p = s + 3;
    return cp;
  }
  if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
      (s[3] & 0xC0) == 0x80) {
    cp = ((unsigned)(s[0] & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) |
         ((unsigned)(s[2] & 0x3F) << 6) | (unsigned)(s[3] & 0x3F);
    *p = s + 4;
    return cp;
  }
  *p = s + 1;
  return (unsigned)'?';
}

static const UiGlyph *glyph_for(unsigned cp) {
  int lo = 0;
  int hi = UI_FONT_N - 1;
  if (cp < 32) cp = (unsigned)'?';
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    unsigned c = ui_font_cp[mid];
    if (c == cp) return &ui_font_glyph[mid];
    if (c < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return &ui_font_glyph['?' - 32];
}

int text_px(const char *s, int scale) {
  const unsigned char *p;
  int w = 0;
  if (!s || scale < 1) return 0;
  p = (const unsigned char *)s;
  while (*p) {
    unsigned cp = utf8_next(&p);
    if (cp < 32) continue;
    w += glyph_for(cp)->advance * scale;
  }
  return w;
}

static int glyph_alpha(const unsigned char *px, int w, int h, int x, int y) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return 0;
  return px[y * w + x];
}

/* 8.8 fixed point. Negative values stay defined; a right shift of a negative is not. */
static void texel_at(int fixed, int *i0, int *frac) {
  if (fixed >= 0) {
    *i0 = fixed >> 8;
    *frac = fixed & 255;
    return;
  }
  {
    int n = -fixed;
    int q = n >> 8;
    int r = n & 255;
    if (r == 0) {
      *i0 = -q;
      *frac = 0;
    } else {
      *i0 = -q - 1;
      *frac = 256 - r;
    }
  }
}

void draw_text(Draw *d, int x, int y, int scale, int r, int g, int b, const char *s) {
  const unsigned char *p;
  int pen;
  int dest;
  if (!s || scale < 1 || !d->surf) return;
  pen = d->ox + (int)(x * d->s);
  y = d->oy + (int)(y * d->s);
  dest = (int)(scale * d->s);
  if (dest < 1) dest = 1;
  p = (const unsigned char *)s;
  while (*p) {
    unsigned cp = utf8_next(&p);
    const UiGlyph *gl;
    const unsigned char *px;
    int row, col;
    int dw, dh;
    if (cp < 32) continue;
    gl = glyph_for(cp);
    px = ui_font_px + gl->off;
    dw = gl->w * dest;
    dh = gl->h * dest;
    if (dest == 1) {
      for (row = 0; row < gl->h; row++) {
        for (col = 0; col < gl->w; col++) {
          int a = px[row * gl->w + col];
          if (a < 12) continue;
          put_px(d, pen + gl->xoff + col, y + gl->yoff + row, r, g, b, a);
        }
      }
    } else if (dw > 0 && dh > 0) {
      /* Sample the outline instead of stamping blocks, so headlines stay smooth. */
      for (row = 0; row < dh; row++) {
        int sy = (int)(((long)(row * 2 + 1) * gl->h * 128) / dh) - 128;
        int y0, fy;
        texel_at(sy, &y0, &fy);
        for (col = 0; col < dw; col++) {
          int sx = (int)(((long)(col * 2 + 1) * gl->w * 128) / dw) - 128;
          int x0, fx;
          int a00, a10, a01, a11, a0, a1, a;
          texel_at(sx, &x0, &fx);
          a00 = glyph_alpha(px, gl->w, gl->h, x0, y0);
          a10 = glyph_alpha(px, gl->w, gl->h, x0 + 1, y0);
          a01 = glyph_alpha(px, gl->w, gl->h, x0, y0 + 1);
          a11 = glyph_alpha(px, gl->w, gl->h, x0 + 1, y0 + 1);
          a0 = a00 + ((a10 - a00) * fx) / 256;
          a1 = a01 + ((a11 - a01) * fx) / 256;
          a = a0 + ((a1 - a0) * fy) / 256;
          if (a < 12) continue;
          put_px(d, pen + gl->xoff * dest + col, y + gl->yoff * dest + row, r, g, b, a);
        }
      }
    }
    pen += gl->advance * dest;
  }
}

void blit_cover(Draw *d, int x, int y, int size, const unsigned char *px, int sw, int sh) {
  int x0, y0, pw, prad, side, sx0, sy0, row;
  if (!d->surf || !px || size < 1 || sw < 1 || sh < 1 || d->bpp != 4 || !d->surf->pixels) return;
  x0 = d->ox + (int)(x * d->s);
  y0 = d->oy + (int)(y * d->s);
  pw = (int)(size * d->s);
  if (pw < 1) pw = 1;
  prad = (int)((size > 200 ? 28 : size > 100 ? 20 : 16) * d->s);
  if (prad < 1) prad = 1;
  if (prad > pw / 2) prad = pw / 2;
  side = sw < sh ? sw : sh;
  sx0 = (sw - side) / 2;
  sy0 = (sh - side) / 2;
  for (row = 0; row < pw; row++) {
    int inset = 0;
    int col;
    if (row < prad) inset = round_inset(row, prad);
    else if (row >= pw - prad) inset = round_inset(pw - 1 - row, prad);
    for (col = inset; col < pw - inset; col++) {
      int sx = sx0 + (col * side) / pw;
      int sy = sy0 + (row * side) / pw;
      const unsigned char *s;
      if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
      s = px + ((size_t)sy * (size_t)sw + (size_t)sx) * 4u;
      put_px(d, x0 + col, y0 + row, s[0], s[1], s[2], s[3] ? s[3] : 255);
    }
  }
}
