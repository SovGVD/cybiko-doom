#ifndef RENDER_H
#define RENDER_H
#include "doomtype.h"

#define SCREENW 160
#define SCREENH 100
#define ROWBYTES 40
#define VIEWW 80                /* columns, two pixels wide each */
#define VIEWH 84                /* rows of 3D view; the status bar is below */
#define CENTERY (VIEWH / 2)

/* the screen: 100 rows of 40 bytes, four 2-bit pixels a byte, leftmost in
 * the top bits, 0 white .. 3 black; row 0 is the top */
extern u8 fb[SCREENH * ROWBYTES];

/* shades 0 (white) .. 6 (black): odd ones are 2x2 checkerboards */
#define SHADES 7

void render_init(void);
void render_view(void);         /* the 3D view of player.mo */
void render_automap(void);


/* drawing helpers (draw.c) */
void fill_rect(int x, int y, int w, int h, int shade);     /* pixels */
void draw_text(int x, int y, const char *s, int shade);    /* 4x6 font */
int text_width(const char *s);
void draw_num(int x, int y, s16 v, int digits, int shade);
void draw_line(int x0, int y0, int x1, int y1, int shade); /* pixels */
void draw_big_text(int x, int y, const char *s, int shade); /* 2x */

#endif
