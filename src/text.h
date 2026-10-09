#ifndef YTM_TEXT_H
#define YTM_TEXT_H

#include <SDL.h>

typedef struct Draw {
  SDL_Surface *surf;
  float s;
  int ox, oy;
} Draw;

void draw_begin(Draw *d, SDL_Surface *surf);
void fill_v(Draw *d, int x, int y, int w, int h, int r, int g, int b);
void draw_text(Draw *d, int x, int y, int scale, int r, int g, int b, const char *s);
int text_px(const char *s, int scale);

#endif
