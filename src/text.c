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

int text_px(const char *s, int scale) {
  int w = 0;
  if (!s || scale < 1) return 0;
  for (int i = 0; s[i]; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch < 32 || ch > 126) ch = '?';
    w += ui_font_glyph[ch - 32].advance * scale;
  }
  return w;
}

void draw_text(Draw *d, int x, int y, int scale, int r, int g, int b, const char *s) {
  int pen;
  if (!s || scale < 1 || !d->surf) return;
  pen = d->ox + (int)(x * d->s);
  y = d->oy + (int)(y * d->s);
  scale = (int)(scale * d->s);
  if (scale < 1) scale = 1;
  for (int i = 0; s[i]; i++) {
    unsigned char ch = (unsigned char)s[i];
    const UiGlyph *gl;
    const unsigned char *px;
    if (ch < 32 || ch > 126) ch = '?';
    gl = &ui_font_glyph[ch - 32];
    px = ui_font_px + gl->off;
    for (int row = 0; row < gl->h; row++) {
      for (int col = 0; col < gl->w; col++) {
        int a = px[row * gl->w + col];
        int dx, dy;
        if (a < 12) continue;
        for (dy = 0; dy < scale; dy++) {
          for (dx = 0; dx < scale; dx++) {
            put_px(d, pen + (gl->xoff + col) * scale + dx,
                   y + (gl->yoff + row) * scale + dy, r, g, b, a);
          }
        }
      }
    }
    pen += gl->advance * scale;
  }
}
