#include "text.h"

#include "font8x8_basic.h"

#include <string.h>

void draw_begin(Draw *d, SDL_Surface *surf) {
  int w = surf ? surf->w : 1920;
  int h = surf ? surf->h : 1080;
  float sx, sy;
  if (w < 16) w = 1920;
  if (h < 16) h = 1080;
  sx = (float)w / 1920.0f;
  sy = (float)h / 1080.0f;
  d->surf = surf;
  d->s = sx < sy ? sx : sy;
  d->ox = (int)((w - 1920.0f * d->s) * 0.5f);
  d->oy = (int)((h - 1080.0f * d->s) * 0.5f);
}

void fill_v(Draw *d, int x, int y, int w, int h, int r, int g, int b) {
  SDL_Rect rc;
  if (!d->surf) return;
  rc.x = d->ox + (int)(x * d->s);
  rc.y = d->oy + (int)(y * d->s);
  rc.w = (int)(w * d->s);
  rc.h = (int)(h * d->s);
  if (rc.w < 1) rc.w = 1;
  if (rc.h < 1) rc.h = 1;
  SDL_FillRect(d->surf, &rc, SDL_MapRGB(d->surf->format, (Uint8)r, (Uint8)g, (Uint8)b));
}

int text_px(const char *s, int scale) {
  if (!s) return 0;
  return (int)strlen(s) * 8 * scale;
}

void draw_text(Draw *d, int x, int y, int scale, int r, int g, int b, const char *s) {
  Uint32 color;
  if (!s || scale < 1 || !d->surf) return;
  color = SDL_MapRGB(d->surf->format, (Uint8)r, (Uint8)g, (Uint8)b);
  for (int i = 0; s[i]; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch >= 128) ch = '?';
    const unsigned char *glyph = (const unsigned char *)font8x8_basic[ch];
    for (int row = 0; row < 8; row++) {
      unsigned char bits = glyph[row];
      for (int col = 0; col < 8; col++) {
        SDL_Rect px;
        if ((bits & (1u << col)) == 0) continue;
        px.x = d->ox + (int)((x + (i * 8 + col) * scale) * d->s);
        px.y = d->oy + (int)((y + row * scale) * d->s);
        px.w = (int)(scale * d->s);
        px.h = (int)(scale * d->s);
        if (px.w < 1) px.w = 1;
        if (px.h < 1) px.h = 1;
        SDL_FillRect(d->surf, &px, color);
      }
    }
  }
}
