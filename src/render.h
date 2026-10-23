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
/* In high detail two columns share a byte of fb, so the odd columns go
 * into fb2 instead (each buffer's other half-bytes stay 0, so no byte is
 * read before it is written) and hw_present or's the two for rows
 * fb_merge_y0..fb_merge_y1-1 while fb_merge is set. */
extern u8 fb2[SCREENH * ROWBYTES];
extern u8 fb_merge;
extern s16 fb_merge_y0, fb_merge_y1;
extern u8 show_weapon;          /* draw the player's weapon (the title menu) */
extern u8 show_textures;        /* walls: 0 flat, 1 textured, 2 textured near (the title menu) */
extern u8 interlace;            /* every other column a frame (the title menu) */
extern u8 smooth_near;          /* interpolate the texels of magnified walls (the title menu) */
extern u8 view_size;            /* 0 the whole view, 1 a 128x68 window, 2 96x52 (the title menu) */
extern u8 low_detail;           /* 1: 40 columns of 4 pixels, 2: and 2 rows a texel (the title menu) */

/* data the renderer uses most goes into the H8S's fast on-chip RAM
 * (cybiko/link.ld); it is set up at run time, never initialised */
#ifdef __H8300S__
#define IRAM __attribute__((section(".iram")))
/* the renderer's hottest functions also run from on-chip RAM: hw_init
 * copies the .iramfn section there and link.ld names the copies, so a
 * call to name() reaches the copy (name_img is the one in the image) */
#define IRAM_FN(ret, name, args) \
    ret name args; \
    ret name##_img args __attribute__((section(".iramfn"))); \
    ret name##_img args
#else
#define IRAM
#define IRAM_FN(ret, name, args) static ret name args
#endif

/* shades 0 (white) .. 6 (black): odd ones are 2x2 checkerboards */
#define SHADES 7

void render_init(void);
void render_set_view(void);     /* after view_size changes */
void render_border_dirty(void); /* something was drawn over the view's frame */
s16 render_rows_from(void);     /* the first screen row render_view changed */
void render_drawn_over(s16 x, s16 y, s16 w, s16 h);  /* fb was drawn on over the view */
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
