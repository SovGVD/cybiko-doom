/* The 3D view: Doom's BSP renderer cut down for a 160x100, four-grey screen.
 *
 * Floors and ceilings are flat shades: each flat has one grey tone (its
 * average brightness, worked out by tools/wad2cy.py), dimmed by the
 * sector's light and the distance, and drawn with 2x2 dithering for seven
 * shades. Walls are textured, with the textures shrunk to a texel per 4x4
 * pixels in 16 tones (or, chosen in the title menu, flat shades like the
 * floors). Edges of walls, steps and openings are drawn black so the shapes
 * read well. Columns are two pixels wide (Doom's "low detail"), 80 across.
 *
 * Coordinates: map units; the view transform works in half units (Q1) so
 * that it stays in 16 x 16 bit multiplies, which the H8S does in one
 * instruction. Depth z and lateral x are in Q1; screen x is in 1/16 pixel
 * (Q4); screen y is in 16.16; "scale" is 160 * 1024 / z (pixels per unit
 * Q10, times two).
 *
 * Things are their sprites shrunk to cells of 16 tones (wad2cy.py), drawn
 * in shades with a black outline. They are
 * collected while the BSP is walked front to back: when a subsector is
 * reached, the open window of each screen column (between the nearer
 * ceilings and floors already drawn) is exactly what hides the thing, so it
 * is copied then, and the sprites are drawn last, far ones first. */
#include "render.h"
#include "game.h"
#include "level.h"

u8 fb[SCREENH * ROWBYTES], fb2[SCREENH * ROWBYTES];
u8 fb_merge;
s16 fb_merge_y0, fb_merge_y1;

#define NEARZ 8                 /* Q1: 4 map units */
#define MAXCOORD 23000          /* Q1 deltas past this are not drawn */

/* view */
static s32 vx, vy;              /* Q1 */
static s16 viewz;               /* units */
static s16 vcos, vsin;          /* Q14 */

/* column clips: rows still open are ceilclip+1 .. floorclip-1 */
/* The dither bytes of a texel's 16 tones for even and odd rows at 32
 * lights (render_init), first in the on-chip RAM at a 32-byte boundary
 * (link.ld checks): tline takes a byte by setting the low address byte. */
u8 tex_luts[32][2][16] IRAM;
u8 low_detail;                  /* 1: columns of 4 pixels, 2: and 2 rows a texel */
u8 interlace = 1;               /* every other column a frame (the title menu;
                                 * on by default: a real Cybiko's slow FSTN
                                 * screen smears the interlacing away) */
u8 view_size;
/* The 3D view's window on the screen (render_set_view): a smaller one has
 * fewer pixels to fill. Columns are numbered on the screen (two or four
 * pixels each), so those below col0 or from vieww up are outside it. */
static s16 view_px0, view_py0, view_pw, view_ph;    /* in pixels and rows */
static s16 focal;               /* half the window's width in pixels */
static s16 xoff;                /* Q4 pixels: the window's centre */
static s16 centery, viewbot;    /* the centre row and the first row below */
static s32 yscale;              /* focal in the units of scale (see yproj) */
static s16 near_scale;          /* scale at 512 units, for TEXTURED NEAR */
static s16 col0;                /* the window's first column */
static u8 col_skip;             /* 1: skip the columns whose parity is not col_par */
static u8 col_par;
static s16 vieww;               /* the first column past the window */
static u8 colbits;              /* log2 of a column's width in Q4 pixels */
static s8 ceilclip[VIEWW] IRAM, floorclip[VIEWW] IRAM;

/* shades */
u8 show_weapon = 1;
u8 show_textures = 1;
u8 smooth_near = 1;             /* blend texels on magnified walls (the title menu) */
u8 pat[SHADES][2] IRAM;       /* also used by cybiko/rt.s */
static u8 shadelut[256] IRAM;   /* brightness -> shade */
static u8 dimtab[64] IRAM;      /* distance dimming by scale >> 4, of 256 */

/* ---- vertex transform cache ---- */
#define MAXVERTS 2048
static s16 tvx[MAXVERTS], tvz[MAXVERTS], tdx[MAXVERTS], tdy[MAXVERTS];
static u16 tstamp[MAXVERTS];
static u16 framecount;

/* 0 if the vertex is too far away to draw */
static int transform(s16 v)
{
    s32 dx, dy;
    if (tstamp[v] == framecount) return tdx[v] != -32768;
    tstamp[v] = framecount;
    dx = ((s32)lvl.vertexes[v].x << 1) - vx;
    dy = ((s32)lvl.vertexes[v].y << 1) - vy;
    if (dx < -MAXCOORD || dx > MAXCOORD || dy < -MAXCOORD || dy > MAXCOORD) {
        tdx[v] = -32768;
        return 0;
    }
    tdx[v] = (s16)dx;
    tdy[v] = (s16)dy;
    tvz[v] = (s16)((MUL16(dx, vcos) + MUL16(dy, vsin)) >> 14);
    tvx[v] = (s16)((MUL16(dx, vsin) - MUL16(dy, vcos)) >> 14);
    return 1;
}

/* ---- frustum clipping of a view-space segment ---- */

/* t (Q14) where d goes from d1 to d2 through 0 */
static s16 clip_t(s32 d1, s32 d2)
{
    s32 den = d1 - d2;
    while (den > 32767 || den < -32767 || d1 > 32767 || d1 < -32767) {
        d1 >>= 1;
        den >>= 1;
    }
    if (!den) return 0;
    return div32_16(d1 << 14, (s16)den);
}

/* clip (x1,z1)-(x2,z2) to z >= NEARZ, -z <= x <= z; 0 if nothing is left.
 * *c1 / *c2 are set when an end was cut. */
IRAM_FN(int, clip_seg, (s16 *x1, s16 *z1, s16 *x2, s16 *z2, s16 *u1, s16 *u2, u8 *c1, u8 *c2))
{
    int pass;
    for (pass = 0; pass < 3; pass++) {
        s32 d1, d2;
        s16 t;
        if (pass == 0) { d1 = *z1 - NEARZ; d2 = *z2 - NEARZ; }
        else if (pass == 1) { d1 = (s32)*z1 + *x1; d2 = (s32)*z2 + *x2; }
        else { d1 = (s32)*z1 - *x1; d2 = (s32)*z2 - *x2; }
        if (d1 < 0 && d2 < 0) return 0;
        if (d1 >= 0 && d2 >= 0) continue;
        t = clip_t(d1, d2);
        if (d1 < 0) {
            *x1 = (s16)(*x1 + (MUL16(*x2 - *x1, t) >> 14));
            *z1 = (s16)(*z1 + (MUL16(*z2 - *z1, t) >> 14));
            *u1 = (s16)(*u1 + (MUL16(*u2 - *u1, t) >> 14));
            *c1 = 1;
        } else {
            *x2 = (s16)(*x1 + (MUL16(*x2 - *x1, t) >> 14));
            *z2 = (s16)(*z1 + (MUL16(*z2 - *z1, t) >> 14));
            *u2 = (s16)(*u1 + (MUL16(*u2 - *u1, t) >> 14));
            *c2 = 1;
        }
        if (*z1 < NEARZ) *z1 = NEARZ;
        if (*z2 < NEARZ) *z2 = NEARZ;
    }
    return 1;
}

/* screen x (Q4) of a clipped point */
static s16 project_x(s16 x, s16 z)
{
    return (s16)(xoff + div32_16(MUL16(x, focal * 16), z));
}

/* ---- solid ranges (Doom's solidsegs) ---- */
typedef struct { s16 first, last; } cliprange_t;
#define MAXSEGS 48
static cliprange_t solidsegs[MAXSEGS] IRAM, *newend;

static void clear_clips(void)
{
    solidsegs[0].first = -0x7fff;
    solidsegs[0].last = col0 - 1;
    solidsegs[1].first = vieww;
    solidsegs[1].last = 0x7fff;
    newend = solidsegs + 2;
}

/* ---- the wall being drawn ---- */
static struct {
    const seg_t *seg;
    sector_t *front, *back;
    s16 sx1, sx2;               /* Q4 screen x of the ends */
    s16 c0, c1;                 /* first and last column of the whole seg */
    u8 edge1, edge2;            /* draw a vertical edge at c0 / c1 */
    u8 solid;                   /* one-sided wall */
    u8 markceil, markfloor, drawtop, drawbottom;
    s32 top, topstep;           /* front ceiling, 16.16 row, per column */
    s32 bot, botstep;           /* front floor */
    s32 high, highstep;         /* back ceiling */
    s32 low, lowstep;           /* back floor */
    s32 sc, scstep;             /* scale << 16 */
    u8 bwall, btop, bbot;       /* brightness of the pieces */
    u8 ceilshade, floorshade;
    /* textures */
    s32 us, usstep;             /* texture column (units) times scale */
    s16 vmid, vtop, vbot;       /* height of the textures' top row above the eye */
    const texture_t *tmid, *ttop, *tbot;        /* NULL: flat shade */
    u8 light;
} w;

static s32 yproj(s16 dh, s16 scale)
{
    s32 v = MUL16(dh, scale);           /* rows Q10 */
    if (v > 0x1000000) v = 0x1000000;
    if (v < -0x1000000) v = -0x1000000;
    return ((s32)centery << 16) - (v << 6) + 0x8000;
}

/* value at the centre of column c, given its values at sx1 and sx2 */
static void interp(s32 a, s32 b, s32 *start, s32 *step, s16 c)
{
    s16 span = w.sx2 - w.sx1;
    s32 perq4;
    if (span < 1) span = 1;
    perq4 = ldiv32_16(b - a, span);
    *step = perq4 << colbits;
    *start = a + perq4 * (s32)((s16)((c << colbits) + (1 << (colbits - 1))) - w.sx1);
}

static u8 flat_shade(u8 tone, u8 light)
{
    return shadelut[(u8)(((u16)tone * light) >> 8) * 3 >> 2];
}

/* ---- column filling ---- */
/* a column's byte in row 0 of fb or fb2 (render.h) and its bits of it */
#define COLBUF(x) (low_detail ? fb + (x) : ((x) & 1 ? fb2 : fb) + ((x) >> 1))
#define COLMASK(x) (low_detail ? 0xff : (x) & 1 ? 0x0f : 0xf0)
void vline(s16 x, s16 y1, s16 y2, u8 shade);

#ifndef __H8300S__
void vline(s16 x, s16 y1, s16 y2, u8 shade)
{
    u8 *p, mask, a, b;
    s16 n;
    if (y1 > y2) return;
    p = COLBUF(x) + y1 * ROWBYTES;
    mask = COLMASK(x);
    a = pat[shade][y1 & 1] & mask;
    b = pat[shade][(y1 & 1) ^ 1] & mask;
    n = y2 - y1 + 1;
    while (n >= 2) {
        p[0] = a;
        p[ROWBYTES] = b;
        p += 2 * ROWBYTES;
        n -= 2;
    }
    if (n) p[0] = a;
}
#endif

IRAM_FN(void, dot, (s16 x, s16 y, u8 shade))
{
    COLBUF(x)[y * ROWBYTES] = pat[shade][y & 1] & COLMASK(x);
}

#define EDGE 6

/* ---- textured columns ----
 * A texel has 16 tones; tl_lut (one of tex_luts) has the dither pattern of
 * each tone's shade for even and odd rows at the current light.
 * tline draws rows y1..y2 of column x from a texture column tl_col: texel
 * (tl_v >> 8) & tl_hmask at row y1, then tl_v += tl_step a row (8.8 fixed
 * point; the texture heights divide 256). cybiko/rt.s has it in assembler. */
const u8 (*tl_lut)[16];     /* tex_luts[light >> 3] */
const u8 *tl_col;
u16 tl_v, tl_step;
u8 tl_hmask;
void tline(s16 x, s16 y1, s16 y2);

#ifndef __H8300S__
void tline(s16 x, s16 y1, s16 y2)
{
    u8 *p = COLBUF(x) + y1 * ROWBYTES;
    u8 mask = COLMASK(x), odd = y1 & 1;
    u16 v = tl_v;
    if (low_detail == 2) {          /* one texel for two rows */
        for (; y1 < y2; y1 += 2, p += 2 * ROWBYTES, v += 2 * tl_step) {
            u8 t = tl_col[(u8)(v >> 8) & tl_hmask];
            p[0] = tl_lut[odd][t];
            p[ROWBYTES] = tl_lut[odd ^ 1][t];
        }
        if (y1 == y2) *p = tl_lut[odd][tl_col[(u8)(v >> 8) & tl_hmask]];
        return;
    }
    for (; y1 <= y2; y1++, p += ROWBYTES, v += tl_step) {
        *p = tl_lut[odd][tl_col[(u8)(v >> 8) & tl_hmask]] & mask;
        odd ^= 1;
    }
}
#endif

/* A magnified wall (less than a texel a row) with SMOOTHING on: the texel
 * tones are interpolated, across from this texel column to the next by
 * fu/256 and along it between neighbouring texels, so that the texels do
 * not show as flat blocks right in front of a wall. The tone is stepped
 * per row (8.8) and only recomputed when the row enters a new texel. This
 * runs from DRAM: .iramfn is full, and near walls are the cheap scenes. */

static const texture_t *tl_tex;
static s32 tl_u;

/* tsmooth: 2*pairs rows from p, tone (8.8) stepped by ts_dt a row; the
 * dither bytes are ts_lut[tone >> 8] for the first row, the other half of
 * tex_luts[l] for the next (cybiko/rt.s has it in assembler) */
#define OTHER_HALF(l) ((l) == tl_lut[0] ? tl_lut[1] : tl_lut[0])
const u8 *ts_lut;
s16 ts_dt;
u8 ts_mask;
void tsmooth(u8 *p, s16 pairs, u16 tone);

#ifndef __H8300S__
void tsmooth(u8 *p, s16 pairs, u16 tone)
{
    const u8 *l0 = ts_lut, *l1 = OTHER_HALF(ts_lut);
    for (; pairs > 0; pairs--) {
        p[0] = l0[tone >> 8] & ts_mask;
        tone += ts_dt;
        p[ROWBYTES] = l1[tone >> 8] & ts_mask;
        tone += ts_dt;
        p += 2 * ROWBYTES;
    }
}
#endif

static void __attribute__((noinline)) tline_smooth(s16 x, s16 y1, s16 y2)
{
    const texture_t *t = tl_tex;
    const u8 *c1 = t->texels + (((tl_u >> t->shift) & ((1 << t->wbits) - 1)) << t->hbits);
    const u8 *c2 = t->texels + ((((tl_u >> t->shift) + 1) & ((1 << t->wbits) - 1)) << t->hbits);
    s16 fu = (s16)((tl_u & ((1 << t->shift) - 1)) << (8 - t->shift));
    u8 *p = COLBUF(x) + y1 * ROWBYTES;
    u8 hm = tl_hmask;
    u16 v = (u16)(tl_v - 128), step = tl_step;  /* texel centres */
    s16 n = y2 - y1 + 1;
    ts_lut = tl_lut[y1 & 1];
    ts_mask = COLMASK(x);
    while (n > 0) {
        /* rows left in this texel, and the tone (8.8) along them */
        u8 r = (u8)(v >> 8), r1 = (u8)(r + 1);
        s16 a = c1[r & hm], b = c1[r1 & hm], k;
        u16 tone;
        a += (s16)((c2[r & hm] - a) * fu) >> 8;
        b += (s16)((c2[r1 & hm] - b) * fu) >> 8;
        ts_dt = (s16)((b - a) * (s16)step);         /* tone a row */
        tone = (u16)((a << 8) + (((b - a) * (s16)(v & 255))));
        k = (s16)(((u16)(256 - (v & 255)) + step - 1) / step);
        if (k > n) k = n;
        n -= k;
        v += (u16)(k * step);
        if (k & 1) {
            *p = ts_lut[tone >> 8] & ts_mask;
            p += ROWBYTES;
            tone += ts_dt;
            ts_lut = OTHER_HALF(ts_lut);
        }
        if (k >>= 1) {
            tsmooth(p, k, tone);
            p += k * 2 * ROWBYTES;
        }
    }
}

#define set_tex_light(l) (tl_lut = (const u8 (*)[16])tex_luts[(l) >> 3])

/* rows y1..y2 of a wall piece: texture t, top row at height vtop above the
 * eye; u is the texture column in units, vstep units per row (16.16) */
IRAM_FN(void, tex_piece, (s16 x, s16 y1, s16 y2, const texture_t *t, s16 vtop, s32 u, s32 vstep))
{
    s32 step = vstep >> t->shift;
    if (step > 0xffffffL) step = 0xffffffL;
    tl_col = t->texels + ((((u >> t->shift) & ((1 << t->wbits) - 1))) << t->hbits);
    tl_hmask = (u8)((1 << t->hbits) - 1);
    tl_step = (u16)(step >> 8);
    tl_v = (u16)((((s32)vtop << 8) >> t->shift) + MUL16(y1 - centery, tl_step) - (tl_step >> 1));
    if (tl_step < 64 && smooth_near) {
        tl_tex = t;
        tl_u = u;
        tline_smooth(x, y1, y2);
    } else
        tline(x, y1, y2);
}

IRAM_FN(void, render_columns, (s16 x, s16 stop))
{
    s32 top, bot, high = 0, low = 0, sc, us;
    s32 topstep, botstep, highstep, lowstep, scstep, usstep;
    s16 d, xstep = 1;
    u8 textured = w.tmid || w.ttop || w.tbot;

    if (col_skip) {                     /* interlaced: this frame's columns */
        if ((x ^ col_par) & 1) x++;
        xstep = 2;
    }
    d = x - w.c0;
    /* values at the first column drawn */
    top = w.top + w.topstep * d;
    bot = w.bot + w.botstep * d;
    if (w.drawtop) high = w.high + w.highstep * d;
    if (w.drawbottom) low = w.low + w.lowstep * d;
    sc = w.sc + w.scstep * d;
    us = w.us + w.usstep * d;
    topstep = w.topstep * xstep;
    botstep = w.botstep * xstep;
    highstep = w.highstep * xstep;
    lowstep = w.lowstep * xstep;
    scstep = w.scstep * xstep;
    usstep = w.usstep * xstep;

    for (; x <= stop; x += xstep) {
        s32 u = 0, vstep = 0;
        s16 ctop = ceilclip[x] + 1, cbot = floorclip[x] - 1;
        s16 yl = (s16)(top >> 16), yh = (s16)(bot >> 16);
        s16 s = (s16)(sc >> 20);           /* scale / 16 */
        u8 dim = dimtab[s > 63 ? 63 : (s < 0 ? 0 : s)];
        u8 vedge = (x == w.c0 && w.edge1) || (x == w.c1 && w.edge2);
        u8 topedge = yl >= ctop, botedge = yh <= cbot;
        if (textured && !vedge) {
            s16 scl = (s16)(sc >> 16);
            if (scl < 1) scl = 1;
            u = ldiv32_16(us, scl);
            vstep = ldiv32_16(0x4000000L, scl);     /* units per row, 16.16 */
            set_tex_light((u8)((w.light * dim) >> 8));
        }

        if (yl < ctop) yl = ctop;
        if (yh > cbot) yh = cbot;
        /* and the other way: a wall wholly above or below the open rows
         * would otherwise put a row outside the view into the s8 clips,
         * wrapped, and later columns drew far past fb (writing over the
         * game's other data and CyOS's) */
        if (yl > cbot + 1) yl = cbot + 1;
        if (yh < ctop - 1) yh = ctop - 1;
        if (w.markceil) {
            s16 e = yl - 1;
            if (e > cbot) e = cbot;
            vline(x, ctop, e, w.ceilshade);
        }
        if (w.markfloor) {
            s16 b = yh + 1;
            if (b < ctop) b = ctop;
            vline(x, b, cbot, w.floorshade);
        }
        if (w.solid) {
            if (yl <= yh) {
                if (w.tmid && !vedge) tex_piece(x, yl, yh, w.tmid, w.vmid, u, vstep);
                else vline(x, yl, yh, vedge ? EDGE : shadelut[(w.bwall * dim) >> 8]);
                if (topedge) dot(x, yl, EDGE);
                if (botedge && yh > yl) dot(x, yh, EDGE);
            }
            ceilclip[x] = (s8)viewbot;
            floorclip[x] = -1;
        } else {
            if (w.drawtop) {
                s16 mid = (s16)(high >> 16);
                high += highstep;
                if (mid >= cbot + 1) mid = cbot;
                if (mid >= yl) {
                    if (w.ttop && !vedge) tex_piece(x, yl, mid, w.ttop, w.vtop, u, vstep);
                    else vline(x, yl, mid, vedge ? EDGE : shadelut[(w.btop * dim) >> 8]);
                    if (topedge) dot(x, yl, EDGE);
                    dot(x, mid, EDGE);
                    ceilclip[x] = (s8)mid;
                } else
                    ceilclip[x] = (s8)(yl - 1);
            } else if (w.markceil)
                ceilclip[x] = (s8)(yl - 1);
            if (w.drawbottom) {
                s16 mid = (s16)(low >> 16) + 1;
                low += lowstep;
                if (mid <= ceilclip[x]) mid = ceilclip[x] + 1;
                if (mid <= yh) {
                    if (w.tbot && !vedge) tex_piece(x, mid, yh, w.tbot, w.vbot, u, vstep);
                    else vline(x, mid, yh, vedge ? EDGE : shadelut[(w.bbot * dim) >> 8]);
                    dot(x, mid, EDGE);
                    if (botedge && yh > mid) dot(x, yh, EDGE);
                    floorclip[x] = (s8)mid;
                } else
                    floorclip[x] = (s8)(yh + 1);
            } else if (w.markfloor)
                floorclip[x] = (s8)(yh + 1);
        }
        top += topstep;
        bot += botstep;
        sc += scstep;
        us += usstep;
    }
}

/* ---- clipping a wall against the solid ranges (Doom r_bsp.c) ---- */

static void clip_solid(s16 first, s16 last)
{
    cliprange_t *next, *start = solidsegs;
    while (start->last < first - 1) start++;
    if (first < start->first) {
        if (last < start->first - 1) {
            /* entirely visible: insert a new range */
            next = newend;
            if (newend - solidsegs >= MAXSEGS) return;
            newend++;
            while (next != start) {
                *next = *(next - 1);
                next--;
            }
            next->first = first;
            next->last = last;
            render_columns(first, last);
            return;
        }
        render_columns(first, start->first - 1);
        start->first = first;
    }
    if (last <= start->last) return;
    next = start;
    while (last >= (next + 1)->first - 1) {
        render_columns(next->last + 1, (next + 1)->first - 1);
        next++;
        if (last <= next->last) {
            start->last = next->last;
            goto crunch;
        }
    }
    render_columns(next->last + 1, last);
    start->last = last;
crunch:
    if (next == start) return;
    while (next++ != newend) *++start = *next;
    newend = start + 1;
}

static void clip_pass(s16 first, s16 last)
{
    cliprange_t *start = solidsegs;
    while (start->last < first - 1) start++;
    if (first < start->first) {
        if (last < start->first - 1) {
            render_columns(first, last);
            return;
        }
        render_columns(first, start->first - 1);
    }
    if (last <= start->last) return;
    while (last >= (start + 1)->first - 1) {
        render_columns(start->last + 1, (start + 1)->first - 1);
        start++;
        if (last <= start->last) return;
    }
    render_columns(start->last + 1, last);
}

/* ---- walls ---- */

static u8 bright(u8 tone, u8 light)
{
    if (!tone) tone = 96;
    return (u8)(((u16)tone * light) >> 8);
}

/* scale at 512 units for TEXTURED NEAR: near_scale (render_set_view) */

static void add_seg(const seg_t *seg)
{
    s16 x1, z1, x2, z2, c0, c1, u1, u2;
    u8 cut1 = 0, cut2 = 0;
    sector_t *front = &lvl.sectors[seg->front], *back;
    int solid;

    if (!transform(seg->v1) || !transform(seg->v2)) return;
    /* back-facing? */
    if (MUL16(tdx[seg->v1], tdy[seg->v2]) - MUL16(tdy[seg->v1], tdx[seg->v2]) >= 0) return;
    x1 = tvx[seg->v1];
    z1 = tvz[seg->v1];
    x2 = tvx[seg->v2];
    z2 = tvz[seg->v2];
    u1 = seg->u;
    u2 = seg->u + seg->length;
    if (!clip_seg(&x1, &z1, &x2, &z2, &u1, &u2, &cut1, &cut2)) return;
    w.sx1 = project_x(x1, z1);
    w.sx2 = project_x(x2, z2);
    c0 = (w.sx1 + (1 << colbits) - 1) >> colbits;
    c1 = ((w.sx2 + (1 << colbits) - 1) >> colbits) - 1;
    if (c0 > c1) return;
    if (c0 < col0) c0 = col0;
    if (c1 > vieww - 1) c1 = vieww - 1;
    if (c0 > c1) return;

    back = seg->back >= 0 ? &lvl.sectors[seg->back] : NULL;
    solid = !back || back->ceilh <= front->floorh || back->floorh >= front->ceilh;
    if (back && !solid && back->ceilh == front->ceilh && back->floorh == front->floorh
        && back->ceiltone == front->ceiltone && back->floortone == front->floortone
        && back->light == front->light)
        return;                         /* nothing to see on this line */

    lvl.lines[seg->line].flags |= ML_MAPPED;

    w.seg = seg;
    w.front = front;
    w.back = back;
    w.c0 = c0;
    w.c1 = c1;
    w.edge1 = (seg->edge & 1) && !cut1;
    w.edge2 = (seg->edge & 2) && !cut2;
    w.solid = !back;
    {
        s16 s1 = div32_16(yscale, z1), s2 = div32_16(yscale, z2);
        s16 wt = front->ceilh - viewz, wb = front->floorh - viewz;
        interp(yproj(wt, s1), yproj(wt, s2), &w.top, &w.topstep, c0);
        interp(yproj(wb, s1), yproj(wb, s2), &w.bot, &w.botstep, c0);
        w.bot -= 0x10000;
        interp((s32)s1 << 16, (s32)s2 << 16, &w.sc, &w.scstep, c0);
        w.ceilshade = front->ceiltone ? flat_shade(front->ceiltone, front->light) : 1;
        w.floorshade = flat_shade(front->liquid ? front->floortone >> 1 : front->floortone,
                                  front->light);
        w.bwall = bright(seg->mid, front->light);
        w.light = front->light;
        w.tmid = w.ttop = w.tbot = NULL;
        /* show_textures 2: only near walls (closer than 512 units at
         * either end) are textured */
        if (show_textures == 1 || (show_textures == 2 && (s1 > near_scale || s2 > near_scale))) {
            u16 fl = lvl.lines[seg->line].flags;
            interp(MUL16(u1, s1), MUL16(u2, s2), &w.us, &w.usstep, c0);
            if (!back && seg->tmid < lvl.ntex) {
                w.tmid = &lvl.tex[seg->tmid];
                w.vmid = (fl & ML_DONTPEGBOTTOM) ? front->floorh + (4 << w.tmid->hbits) - viewz : wt;
                w.vmid += seg->yoffset;
            }
            if (back && seg->ttop < lvl.ntex) {
                w.ttop = &lvl.tex[seg->ttop];
                w.vtop = (fl & ML_DONTPEGTOP) ? wt : back->ceilh + (4 << w.ttop->hbits) - viewz;
                w.vtop += seg->yoffset;
            }
            if (back && seg->tbottom < lvl.ntex) {
                w.tbot = &lvl.tex[seg->tbottom];
                w.vbot = (fl & ML_DONTPEGBOTTOM) ? wt : back->floorh - viewz;
                w.vbot += seg->yoffset;
            }
        }
        w.markceil = front->ceilh > viewz || !front->ceiltone;
        w.markfloor = front->floorh < viewz;
        w.drawtop = w.drawbottom = 0;
        if (back) {
            s16 wh = back->ceilh - viewz, wl = back->floorh - viewz;
            if (!front->ceiltone && !back->ceiltone) wt = wh;     /* sky hack */
            if (wh < wt) {
                w.drawtop = 1;
                interp(yproj(wh, s1), yproj(wh, s2), &w.high, &w.highstep, c0);
                w.high -= 0x10000;
                w.btop = bright(seg->top, front->light);
            }
            if (wl > wb) {
                w.drawbottom = 1;
                interp(yproj(wl, s1), yproj(wl, s2), &w.low, &w.lowstep, c0);
                w.low -= 0x10000;
                w.bbot = bright(seg->bottom, front->light);
            }
            if (!solid) {
                if (back->ceilh == front->ceilh && back->ceiltone == front->ceiltone
                    && back->light == front->light)
                    w.markceil = 0;
                if (back->floorh == front->floorh && back->floortone == front->floortone
                    && back->light == front->light)
                    w.markfloor = 0;
            } else {
                w.markceil = w.markfloor = 1;
            }
            if (front->ceilh <= viewz && front->ceiltone) w.markceil = 0;
            if (front->floorh >= viewz) w.markfloor = 0;
        }
    }
    if (solid) clip_solid(c0, c1);
    else clip_pass(c0, c1);
}

/* ---- things ---- */

typedef struct {
    s16 x1, x2;                 /* columns */
    s32 xfrac, xstep;           /* sprite cell column, 16.16, at x1 and per column */
    s32 ytop, ystep;            /* screen row of the top, 16.16; rows per cell */
    const u8 *sil;
    u16 clip;                   /* offset in the clip pool */
    u8 light;                   /* of the thing's sector */
    u8 shadow;
} vissprite_t;

#define MAXVIS 48
#define POOL 2400
static vissprite_t vis[MAXVIS];
static s16 nvis;
static s8 poolc[POOL], poolf[POOL];
static u16 poolused;

/* a grey box for things whose sprite is not in the WAD */
static const u8 box_sil[] = { 1, 4, 8, 0, 4, 0,
    1, 0, 4, 0x38, 0x38, 0x38, 0x38 };

static void add_sprite(mobj_t *mo, const u8 *sil)
{
    s32 dx = (mo->x >> 15) - vx, dy = (mo->y >> 15) - vy;
    s16 tz, tx, scale, f, cw, x1, x2, n;
    s32 left, right;
    vissprite_t *v;
    if (nvis >= MAXVIS) return;
    if (dx < -MAXCOORD || dx > MAXCOORD || dy < -MAXCOORD || dy > MAXCOORD) return;
    tz = (s16)((MUL16(dx, vcos) + MUL16(dy, vsin)) >> 14);
    if (tz < NEARZ * 2) return;
    tx = (s16)((MUL16(dx, vsin) - MUL16(dy, vcos)) >> 14);
    if (tx > tz + 128 || -tx > tz + 128) return;
    if (!sil) sil = box_sil;
    cw = sil[0];
    f = sil[2];
    scale = div32_16(yscale, tz);
    /* left and right edges in Q1 units relative to the thing */
    left = (s32)tx - (s32)(s8)sil[3] * f * 2;
    right = left + (s32)cw * f * 2;
    if (left < -32000 || right > 32000) return;
    /* columns: x = 40 + x*40/z (low detail) */
    {
        s32 lx = ldiv32_16(MUL16((s16)left, focal * 16), tz) + xoff;
        s32 rx = ldiv32_16(MUL16((s16)right, focal * 16), tz) + xoff;
        x1 = (s16)((lx + (1 << colbits) - 1) >> colbits);
        x2 = (s16)(((rx + (1 << colbits) - 1) >> colbits) - 1);
        if (x1 > x2 || x2 < col0 || x1 >= vieww) return;
        v = &vis[nvis];
        /* cells per column */
        v->xstep = ldiv32_16((s32)cw << 16, (s16)((rx - lx) >> colbits ? (rx - lx) >> colbits : 1));
        v->xfrac = 0;
        if (x1 < col0) {
            v->xfrac = v->xstep * (s32)(col0 - x1);
            x1 = col0;
        }
        if (x2 > vieww - 1) x2 = vieww - 1;
    }
    n = x2 - x1 + 1;
    if (poolused + n > POOL) return;
    {
        s16 gz = (s16)(mo->z >> 16) + (s8)sil[4] * f;     /* top of the sprite */
        v->ytop = yproj(gz - viewz, scale);
        v->ystep = (s32)MUL16(f, scale) << 6;          /* rows per cell, 16.16 */
    }
    v->x1 = x1;
    v->x2 = x2;
    v->sil = sil;
    v->clip = poolused;
    v->shadow = (mo->flags & MF_SHADOW) != 0;
    v->light = lvl.sectors[mo->ss->sector].light;
    mcpy(poolc + poolused, ceilclip + x1, (u32)n);
    mcpy(poolf + poolused, floorclip + x1, (u32)n);
    poolused += n;
    nvis++;
}

static const u8 *sprite_frame(mobj_t *mo)
{
    const u8 *s = sprite_get(mo->cls, mo->frame);
    if (!s && mo->frame) s = sprite_get(mo->cls, 0);
    return s;
}

/* the next column of a sprite: skip the runs of this one */
static const u8 *next_column(const u8 *p)
{
    s16 n = *p++;
    while (n--) {
        p += 2 + p[1] - p[0];
    }
    return p;
}

/* sprite tone (0 black .. 15 white) to a shade, in a sector of this light */
static u8 sprite_shade(u8 tone, u8 light)
{
    s16 sh = 4 - (s16)((tone * 5) >> 4);   /* 4 .. 0: black is for the outline */
    if (light < 128) sh++;
    return (u8)(sh > 4 ? 4 : sh);
}

/* State for cell_run, which draws one run of sprite cells in a column; it
 * is written in assembler for the H8S (cybiko/rt.s) and uses these. */
s16 cr_lo, cr_hi;                       /* rows that may be drawn */
u8 *cr_col;                             /* the column's byte in row 0 of fb or fb2 */
u8 cr_mask;                             /* its bits of that byte */
u8 cr_ebase;                            /* 64 x the outline bits that apply */
u8 cr_tab[256] IRAM;                    /* cr_ebase + (cell & 63) -> shade */
s16 rowtab[34] IRAM;                    /* screen row where cell row r starts */
static s16 tab_light = -1;              /* the light cr_tab is made for */

void cell_run(const u8 *c, s16 r, s16 e);

#ifndef __H8300S__
/* cells c of cell rows r..e-1; cells of one shade next to each other
 * become one vline */
void cell_run(const u8 *c, s16 r, s16 e)
{
    const s16 *rt = rowtab + r;
    s16 sa = 0, sb = -1, n = e - r;
    s16 x = (s16)(low_detail ? cr_col - fb : cr_mask == 0x0f ? (cr_col - fb2) * 2 + 1 : (cr_col - fb) * 2);
    u8 ssh = 0;
    for (; n; n--, c++, rt++) {
        s16 ya = rt[0], yb = rt[1] - 1;
        u8 sh;
        if (yb < cr_lo) continue;
        if (ya > cr_hi) break;
        if (yb < ya) continue;
        if (ya < cr_lo) ya = cr_lo;
        if (yb > cr_hi) yb = cr_hi;
        sh = cr_tab[cr_ebase + (*c & 63)];
        if (sh == ssh && ya == sb + 1) {
            sb = yb;
            continue;
        }
        if (sb >= sa) vline(x, sa, sb, ssh);
        sa = ya;
        sb = yb;
        ssh = sh;
    }
    if (sb >= sa) vline(x, sa, sb, ssh);
}
#endif

/* Draw sprite cells over columns x1..x2: column x shows cell column
 * (xfrac + (x - x1) * xstep) >> 16, cell row r starts at screen row
 * (ytop + r * ystep) >> 16. Rows outside top[]..bot[] (per column, or
 * the whole view without them) are not drawn. */
IRAM_FN(void, draw_cells, (const u8 *spr, s16 x1, s16 x2, s32 xfrac, s32 xstep, s32 ytop, s32 ystep,
                           const s8 *top, const s8 *bot, u8 light, u8 shadow))
{
    s16 x, cw = spr[0], ch = spr[1], colx = 0;
    const u8 *col = spr + 6;
    s32 xf = xfrac, yy = ytop;
    if (light != tab_light) {
        u8 lut[16];
        tab_light = light;
        for (x = 0; x < 16; x++) lut[x] = sprite_shade((u8)x, light);
        for (x = 0; x < 256; x++) cr_tab[x] = (x & (x >> 2) & 0x30) ? EDGE : lut[x & 15];
    }
    if (ch > 32) ch = 32;
    for (x = 0; x <= ch; x++, yy += ystep) {
        s32 y = yy >> 16;
        rowtab[x] = (s16)(y < view_py0 - 1 ? view_py0 - 1 : y > viewbot ? viewbot : y);
    }
    for (x = x1; x <= x2; x++, xf += xstep) {
        s16 cx = (s16)(xf >> 16), n;
        const u8 *p;
        if (cx >= cw) cx = cw - 1;
        while (colx < cx) {
            col = next_column(col);
            colx++;
        }
        if (x < col0 || x >= vieww) continue;
        if (col_skip && ((x ^ col_par) & 1)) continue;
        cr_ebase = 0;
        if (x == x1 || (s16)((xf - xstep) >> 16) != cx) cr_ebase |= 0x40;
        if (x == x2 || (s16)((xf + xstep) >> 16) != cx) cr_ebase |= 0x80;
        cr_lo = view_py0;
        cr_hi = viewbot - 1;
        if (top) {
            cr_lo = top[x - x1] + 1;
            cr_hi = bot[x - x1] - 1;
            if (cr_lo < view_py0) cr_lo = view_py0;
            if (cr_hi > viewbot - 1) cr_hi = viewbot - 1;
            if (cr_lo > cr_hi) continue;
        }
        cr_col = COLBUF(x);
        cr_mask = COLMASK(x);
        p = col;
        n = *p++;
        while (n--) {
            s16 r = p[0], e = p[1], y, rtop, rbot;
            const u8 *c = p + 2;
            p = c + (e - r);
            if (e > ch) e = ch;
            if (r >= e) continue;
            rtop = rowtab[r];
            rbot = rowtab[e] - 1;
            if (shadow) {
                for (y = rtop & ~1; y <= rbot; y += 2)
                    if (y >= cr_lo && y <= cr_hi) dot(x, y, EDGE);
                continue;
            }
            cell_run(c, r, e);
            if (rtop >= cr_lo && rtop <= cr_hi) dot(x, rtop, EDGE);
            if (rbot >= cr_lo && rbot <= cr_hi) dot(x, rbot, EDGE);
        }
    }
}

static void draw_sprite(vissprite_t *v)
{
    draw_cells(v->sil, v->x1, v->x2, v->xfrac, v->xstep, v->ytop, v->ystep,
               poolc + v->clip, poolf + v->clip, v->light, v->shadow);
}

/* ---- BSP ---- */

static void render_subsector(u16 num)
{
    subsector_t *ss = &lvl.subsectors[num];
    const seg_t *seg = &lvl.segs[ss->firstseg];
    s16 n = ss->numsegs;
    mobj_t *mo;
    for (mo = ss->things; mo; mo = mo->snext)
        if (mo != player.mo && !(mo->flags & MF_NODRAW)) add_sprite(mo, sprite_frame(mo));
    while (n--) add_seg(seg++);
}

static const u8 checkcoord[12][4] = {
    {3, 0, 2, 1}, {3, 0, 2, 0}, {3, 1, 2, 0}, {0},
    {2, 0, 2, 1}, {0, 0, 0, 0}, {3, 1, 3, 0}, {0},
    {2, 0, 3, 1}, {2, 1, 3, 1}, {2, 1, 3, 0}, {0}
};

/* is any part of the box visible and not yet covered? */
static int check_bbox(const s16 *bbox)
{
    s16 bx, by, boxpos, x1, z1, x2, z2, c0, c1;
    s32 dx1, dy1, dx2, dy2;
    u8 cut1 = 0, cut2 = 0;
    s16 u1 = 0, u2 = 0;
    cliprange_t *start;
    s16 px = (s16)(vx >> 1), py = (s16)(vy >> 1);

    if (px <= bbox[BOXLEFT]) bx = 0;
    else if (px < bbox[BOXRIGHT]) bx = 1;
    else bx = 2;
    if (py >= bbox[BOXTOP]) by = 0;
    else if (py > bbox[BOXBOTTOM]) by = 1;
    else by = 2;
    boxpos = (by << 2) + bx;
    if (boxpos == 5) return 1;

    dx1 = ((s32)bbox[checkcoord[boxpos][0]] << 1) - vx;
    dy1 = ((s32)bbox[checkcoord[boxpos][1]] << 1) - vy;
    dx2 = ((s32)bbox[checkcoord[boxpos][2]] << 1) - vx;
    dy2 = ((s32)bbox[checkcoord[boxpos][3]] << 1) - vy;
    if (dx1 < -MAXCOORD || dx1 > MAXCOORD || dy1 < -MAXCOORD || dy1 > MAXCOORD ||
        dx2 < -MAXCOORD || dx2 > MAXCOORD || dy2 < -MAXCOORD || dy2 > MAXCOORD)
        return 1;
    z1 = (s16)((MUL16(dx1, vcos) + MUL16(dy1, vsin)) >> 14);
    x1 = (s16)((MUL16(dx1, vsin) - MUL16(dy1, vcos)) >> 14);
    z2 = (s16)((MUL16(dx2, vcos) + MUL16(dy2, vsin)) >> 14);
    x2 = (s16)((MUL16(dx2, vsin) - MUL16(dy2, vcos)) >> 14);
    if (!clip_seg(&x1, &z1, &x2, &z2, &u1, &u2, &cut1, &cut2)) return 0;
    c0 = (project_x(x1, z1) + (1 << colbits) - 1) >> colbits;
    c1 = ((project_x(x2, z2) + (1 << colbits) - 1) >> colbits) - 1;
    if (c0 > c1) {
        s16 t = c0; c0 = c1; c1 = t;
    }
    if (c0 < col0) c0 = col0;
    if (c1 > vieww - 1) c1 = vieww - 1;
    if (c0 > c1) return 0;
    start = solidsegs;
    while (start->last < c1) start++;
    if (c0 >= start->first && c1 <= start->last) return 0;
    return 1;
}

/* The BSP walk (Doom R_RenderBSPNode) with its own node stack: the
 * recursion would cost over 100 bytes of the small on-chip stack a level,
 * with render_subsector's locals inlined. Each entry is a node with the
 * side taken in bit 15 (a node number is below NF_SUBSECTOR). */
#define NODESTACK 96
static u16 nodestack[NODESTACK];

static void render_bsp(void)
{
    u16 bspnum = (u16)(lvl.nnodes - 1), sp = 0;
    for (;;) {
        while (!(bspnum & NF_SUBSECTOR)) {
            const node_t *bsp = &lvl.nodes[bspnum];
            s16 px = (s16)(vx >> 1), py = (s16)(vy >> 1);
            int side;
            /* which side is the viewer on (Doom R_PointOnSide, whole units) */
            if (!bsp->dx) side = px <= bsp->x ? bsp->dy > 0 : bsp->dy < 0;
            else if (!bsp->dy) side = py <= bsp->y ? bsp->dx < 0 : bsp->dx > 0;
            else {
                s32 l = MUL16(bsp->dy, (s16)((vx - ((s32)bsp->x << 1)) >> 1));
                s32 r = MUL16((s16)((vy - ((s32)bsp->y << 1)) >> 1), bsp->dx);
                side = r >= l;
            }
            if (sp < NODESTACK) nodestack[sp++] = (u16)(bspnum | side << 15);
            bspnum = bsp->children[side];
        }
        if (bspnum == 0xffff) bspnum = 0;
        render_subsector(bspnum & ~NF_SUBSECTOR);
        /* back up to the nearest node whose far side may still show */
        for (;;) {
            const node_t *bsp;
            int side;
            if (!sp || solidsegs[0].last >= vieww - 1) return;      /* screen full */
            bspnum = nodestack[--sp];
            side = bspnum >> 15;
            bsp = &lvl.nodes[bspnum & ~NF_SUBSECTOR];
            if (check_bbox(bsp->bbox[side ^ 1])) {
                bspnum = bsp->children[side ^ 1];
                break;
            }
        }
    }
}

/* ---- the player's weapon ---- */

static void draw_weapon(void)
{
    static const u8 wcls[8] = { C_W_FIST, C_W_PISTOL, C_W_SHOTGUN, C_W_CHAINGUN,
                                C_W_LAUNCHER, C_W_PLASMA, C_W_BFG, C_W_CHAINSAW };
    const u8 *sil = sprite_get(wcls[player.weapon & 7], player.weaponframe);
    s16 cw, ch, f, x, y0, bobx, boby;
    if (!sil) sil = sprite_get(wcls[player.weapon & 7], 0);
    if (!sil || player.dead) return;
    cw = sil[0];
    ch = sil[1];
    f = sil[2];
    /* Doom places weapon sprites on a 320x200 screen with their offsets;
     * here a cell is f/2 columns... keep it simple: centre it at the bottom */
    bobx = (s16)(MUL16(fsin(gametic * 512), (s16)(player.bob >> 12)) >> 18);
    boby = (s16)((MUL16(fcos(gametic * 1024), (s16)(player.bob >> 12)) >> 18) & 7);
    /* a cell is f/2 screen pixels on the whole screen, f/4 columns (f/8 in
     * low detail) and f/2 rows; a smaller window shrinks it too (f8: Q8) */
    {
        s32 f8 = (s32)f * view_pw * 256 / SCREENW;
        s16 ncols = (s16)((cw * f8) >> (low_detail ? 11 : 10));
        if (ncols < 1) ncols = 1;
        x = (s16)((col0 + vieww) / 2 - ((cw * f8) >> (low_detail ? 12 : 11)) + (low_detail ? bobx >> 1 : bobx));
        y0 = (s16)(viewbot - ((ch * f8) >> 9) + 2 + boby);
        draw_cells(sil, x, x + ncols - 1, 0, ((s32)cw << 16) / ncols, (s32)y0 << 16,
                   f8 << 7, NULL, NULL, lvl.sectors[player.mo->ss->sector].light, 0);
    }
}

/* ---- frame ---- */

static u8 border_dirty = 1;     /* the frame around a smaller window needs drawing */
static u8 last_detail = 0xff;   /* low_detail of the last frame; 0xff: redraw it all */

/* the view's window for view_size, and what depends on its size */
void render_set_view(void)
{
    int i;
    view_pw = view_size == 2 ? 96 : view_size == 1 ? 128 : SCREENW;
    view_ph = view_size == 2 ? 52 : view_size == 1 ? 68 : VIEWH;
    view_px0 = (s16)((SCREENW - view_pw) / 2);
    view_py0 = (s16)((VIEWH - view_ph) / 2);
    viewbot = (s16)(view_py0 + view_ph);
    centery = (s16)(view_py0 + view_ph / 2);
    focal = (s16)(view_pw / 2);
    xoff = (s16)((view_px0 + focal) * 16);
    yscale = (s32)focal * 2048;         /* 163840 for the whole screen */
    near_scale = (s16)(yscale / 512);
    border_dirty = 1;
    /* dimming by distance: scale/16 = 5120 / distance on the whole screen */
    for (i = 0; i < 64; i++) {
        s16 d = (s16)(112 + (s32)i * 640 / focal);     /* s32: int is 16 bits on the H8 */
        dimtab[i] = (u8)(d > 255 ? 255 : d);
    }
}

/* the dark frame around a smaller window, drawn again when something was
 * drawn over it (render_border_dirty) */
void render_border_dirty(void)
{
    border_dirty = 1;
    last_detail = 0xff;
}

static u8 border_drawn;         /* this frame, so the whole view changed */

static void render_border(void)
{
    s16 y;
    u8 *p = fb;
    border_dirty = 0;
    border_drawn = 1;
    if (!view_size) return;
    for (y = 0; y < VIEWH; y++, p += ROWBYTES) {
        u8 b = pat[5][y & 1];
        s16 i;
        if (y < view_py0 || y >= viewbot) {
            for (i = 0; i < ROWBYTES; i++) p[i] = b;
        } else {
            for (i = 0; i < view_px0 / 4; i++) p[i] = b;
            for (i = (view_px0 + view_pw) / 4; i < ROWBYTES; i++) p[i] = b;
        }
    }
}

/* Put the view's last picture into the layout this frame draws in: split
 * into fb / fb2 for high detail, whole bytes in fb otherwise (fb_merge says
 * which it is in). A frame does not always draw every pixel of the view,
 * and what it leaves must be the last picture, not half of it. */
static void fb_layout(void)
{
    s16 i, n;
    u8 *p, *q;
    if (!low_detail && !fb_merge) {
        p = fb + view_py0 * ROWBYTES;
        q = fb2 + view_py0 * ROWBYTES;
        for (n = (s16)(view_ph * ROWBYTES), i = 0; i < n; i++) {
            q[i] = p[i] & 0x0f;
            p[i] &= 0xf0;
        }
    } else if (low_detail && fb_merge) {
        p = fb + fb_merge_y0 * ROWBYTES;
        q = fb2 + fb_merge_y0 * ROWBYTES;
        for (n = (s16)((fb_merge_y1 - fb_merge_y0) * ROWBYTES), i = 0; i < n; i++) p[i] |= q[i];
        fb_merge = 0;
    }
}

s16 render_rows_from(void)
{
    return border_drawn || !view_size ? 0 : view_py0;
}

void render_init(void)
{
    int s, i;
    for (s = 0; s < SHADES; s++) {
        u8 lo = (u8)(s >> 1), hi = (u8)((s + 1) >> 1);
        pat[s][0] = (u8)(lo << 6 | hi << 4 | lo << 2 | hi);
        pat[s][1] = (u8)(hi << 6 | lo << 4 | hi << 2 | lo);
    }
    /* brightness to shade: Doom's textures are mostly dark, so stretch */
    for (i = 0; i < 256; i++) {
        s16 sh = 6 - (s16)((i * 7) / 112);
        shadelut[i] = (u8)(sh < 0 ? 0 : sh > 5 ? 5 : sh);
    }
    render_set_view();
    for (s = 0; s < 32; s++)
        for (i = 0; i < 16; i++) {
            u8 sh = shadelut[(u8)((i * 17 * (s * 8 + 4)) >> 8)];
            tex_luts[s][0][i] = pat[sh][0];
            tex_luts[s][1][i] = pat[sh][1];
        }
}

void render_view(void)
{
    mobj_t *pm = player.mo;
    s16 i;
    framecount++;
    if (!framecount) {
        for (i = 0; i < MAXVERTS; i++) tstamp[i] = 0xffff;
        framecount = 1;
    }
    vx = pm->x >> 15;
    vy = pm->y >> 15;
    viewz = (s16)(player.viewz >> 16);
    vcos = fcos(pm->angle);
    vsin = fsin(pm->angle);
    colbits = low_detail ? 6 : 5;
    col0 = (s16)(view_px0 >> (colbits - 4));
    vieww = (s16)(col0 + (view_pw >> (colbits - 4)));
    for (i = 0; i < VIEWW; i++) {
        ceilclip[i] = (s8)(view_py0 - 1);
        floorclip[i] = (s8)viewbot;
    }
    border_drawn = 0;
    if (border_dirty) render_border();
    fb_layout();
    clear_clips();
    nvis = 0;
    poolused = 0;
    if (lvl.nvertexes > MAXVERTS) return;
    /* alternate columns only when the other half on the screen is from a
     * frame like this one: after a change of detail (AUTO), the automap or
     * a menu, the whole view is drawn */
    col_skip = interlace && low_detail == last_detail;
    last_detail = low_detail;
    col_par = (u8)(framecount & 1);
    render_bsp();
    for (i = nvis - 1; i >= 0; i--) draw_sprite(&vis[i]);
    col_skip = 0;
    if (show_weapon) draw_weapon();
    fb_merge = !low_detail;
    fb_merge_y0 = view_py0;
    fb_merge_y1 = viewbot;
}

/* something was drawn into fb over the view: take the odd columns there
 * out of fb2, so that merging shows the drawing alone */
void render_drawn_over(s16 x, s16 y, s16 w, s16 h)
{
    s16 b0 = (s16)(x >> 2), b1 = (s16)((x + w - 1) >> 2), i;
    u8 m0 = (u8)(0xff >> 2 * (x & 3)), m1 = (u8)(0xff << 2 * (3 - ((x + w - 1) & 3)));
    if (!fb_merge || w <= 0) return;
    if (b0 == b1) m0 &= m1;
    for (; h > 0; h--, y++) {
        u8 *p;
        if (y < fb_merge_y0 || y >= fb_merge_y1) continue;
        p = fb2 + y * ROWBYTES;
        p[b0] &= (u8)~m0;
        for (i = b0 + 1; i < b1; i++) p[i] = 0;
        if (b1 > b0) p[b1] &= (u8)~m1;
    }
}
