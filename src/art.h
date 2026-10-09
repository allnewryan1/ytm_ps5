#ifndef YTM_ART_H
#define YTM_ART_H

/* Decode a JPEG into a square RGBA buffer of side*side*4. 0 on success. */
int art_jpeg_square(const unsigned char *jpg, int n, unsigned char *dst, int side);

#endif
