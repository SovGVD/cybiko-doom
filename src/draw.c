/* 2D drawing on the full-resolution screen: rectangles, lines, text. */
#include "render.h"

static void pset(int x, int y, int shade)
{
    u8 *p, v, sh;
    if ((unsigned)x >= SCREENW || (unsigned)y >= SCREENH) return;
    p = fb + y * ROWBYTES + (x >> 2);
    sh = (u8)(6 - 2 * (x & 3));
    /* shade -> pixel value, odd shades dithered */
    v = (u8)((shade + ((shade & 1) && ((x ^ y) & 1))) >> 1);
    *p = (u8)((*p & ~(3 << sh)) | v << sh);
}

/* the same pixels as pset() for a whole rectangle, a byte at a time */
void fill_rect(int x, int y, int w, int h, int shade)
{
    int x1 = x + w, y1 = y + h, b0, b1, j;
    u8 m0, m1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SCREENW) x1 = SCREENW;
    if (y1 > SCREENH) y1 = SCREENH;
    if (x >= x1 || y >= y1) return;
    b0 = x >> 2;
    b1 = (x1 - 1) >> 2;
    m0 = (u8)(0xff >> 2 * (x & 3));                 /* pixels of the first byte */
    m1 = (u8)(0xff << 2 * (3 - ((x1 - 1) & 3)));    /* and of the last */
    if (b0 == b1) m0 &= m1;
    for (j = y; j < y1; j++) {
        /* even and odd pixels of the row; odd shades are dithered */
        u8 ve = (u8)((shade + ((shade & 1) && (j & 1))) >> 1);
        u8 vo = (u8)((shade + ((shade & 1) && !(j & 1))) >> 1);
        u8 v = (u8)(ve << 6 | vo << 4 | ve << 2 | vo), *p = fb + j * ROWBYTES + b0;
        int b;
        *p = (u8)((*p & ~m0) | (v & m0));
        if (b0 == b1) continue;
        for (b = b0 + 1, p++; b < b1; b++) *p++ = v;
        *p = (u8)((*p & ~m1) | (v & m1));
    }
}

void draw_line(int x0, int y0, int x1, int y1, int shade)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 400;
    while (n--) {
        int e2;
        pset(x0, y0, shade);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 3x5 glyphs, rows top to bottom, 3 bits each (bit 2 = left) */
static const u16 font[] = {
    0x0000, 0x2482, 0x5a00, 0x5f7d, 0x3c9e, 0x52a5, 0x2aab, 0x2400,
    0x1491, 0x4494, 0x0aa8, 0x05d0, 0x0014, 0x01c0, 0x0002, 0x12a4,
    0x7b6f, 0x2c97, 0x73e7, 0x72cf, 0x5bc9, 0x79cf, 0x79ef, 0x7292,
    0x7bef, 0x7bcf, 0x0410, 0x0414, 0x1511, 0x0e38, 0x4454, 0x72c2,
    0x2be3, 0x2bed, 0x6bae, 0x3923, 0x6b6e, 0x79a7, 0x79a4, 0x396b,
    0x5bed, 0x7497, 0x126a, 0x5bad, 0x4927, 0x5fed, 0x6b6d, 0x2b6a,
    0x6ba4, 0x2b73, 0x6bad, 0x388e, 0x7492, 0x5b6f, 0x5b6a, 0x5bfd,
    0x5aad, 0x5a92, 0x72a7,
};

static int glyph(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c == '_') return 0x0007;
    if (c < ' ' || c > 'Z') return 0;
    return font[c - ' '];
}

static void draw_char(int x, int y, char c, int shade, int k)
{
    int g = glyph(c), r, b;
    for (r = 0; r < 5; r++)
        for (b = 0; b < 3; b++)
            if (g & (1 << (14 - r * 3 - b))) {
                if (k == 1) pset(x + b, y + r, shade);
                else fill_rect(x + b * k, y + r * k, k, k, shade);
            }
}

void draw_text(int x, int y, const char *s, int shade)
{
    for (; *s; s++, x += 4) draw_char(x, y, *s, shade, 1);
}

void draw_big_text(int x, int y, const char *s, int shade)
{
    for (; *s; s++, x += 8) draw_char(x, y, *s, shade, 2);
}

int text_width(const char *s)
{
    int n = 0;
    while (*s++) n += 4;
    return n;
}

void draw_num(int x, int y, s16 v, int digits, int shade)
{
    char buf[8];
    int i = 7, neg = v < 0;
    buf[7] = 0;
    if (neg) v = -v;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 1);
    if (neg) buf[--i] = '-';
    while (7 - i < digits && i > 0) buf[--i] = ' ';
    draw_text(x, y, buf + i, shade);
}
