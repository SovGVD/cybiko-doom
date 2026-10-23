#ifndef INFLATE_H
#define INFLATE_H
#include "doomtype.h"

/* Unpacks raw Deflate data src[0..srclen) into exactly dstlen bytes at dst;
 * 1 if it worked. dst may overlap the end of src (see inflate.c). */
int inflate(void *dst, u32 dstlen, const void *src, u32 srclen);

#endif
