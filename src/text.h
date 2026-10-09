#ifndef YTM_TEXT_H
#define YTM_TEXT_H

#include <SDL.h>

typedef struct Draw {
  SDL_Surface *surf;
  float s;
  int ox, oy;
  int locked;
  int bpp;
  int rshift, gshift, bshift;
  unsigned amask;
} Draw;

void draw_begin(Draw *d, SDL_Surface *surf);
void draw_end(Draw *d);
void fill_v(Draw *d, int x, int y, int w, int h, int r, int g, int b);
void fill_round(Draw *d, int x, int y, int w, int h, int rad, int r, int g, int b);
void draw_text(Draw *d, int x, int y, int scale, int r, int g, int b, const char *s);
int text_px(const char *s, int scale);
/* Center-cropped square cover. px is tightly packed RGBA. */
void blit_cover(Draw *d, int x, int y, int size, const unsigned char *px, int sw, int sh);

#endif
