/* The 3D view: Doom's BSP renderer cut down for a 160x100, four-grey screen.
 *
 * Walls, floors and ceilings are flat shades instead of textures: each wall
 * texture and flat has one grey tone (its average brightness, worked out by
 * tools/wad2cy.py), dimmed by the sector's light and the distance, and drawn
 * with 2x2 dithering for seven shades. Edges of walls, steps and openings
 * are drawn black so the shapes read well. Columns are two pixels wide
 * (Doom's "low detail"), 80 across.
 *
 * Coordinates: map units; the view transform works in half units (Q1) so
 * that it stays in 16 x 16 bit multiplies, which the H8S does in one
 * instruction. Depth z and lateral x are in Q1; screen x is in 1/16 pixel
 * (Q4); screen y is in 16.16; "scale" is 160 * 1024 / z (pixels per unit
 * Q10, times two).
 *
 * Things are drawn as outlines of their sprite silhouettes. They are
 * collected while the BSP is walked front to back: when a subsector is
 * reached, the open window of each screen column (between the nearer
 * ceilings and floors already drawn) is exactly what hides the thing, so it
 * is copied then, and the outlines are drawn last, far ones first. */
#include "render.h"
#include "game.h"
#include "level.h"

u8 fb[SCREENH * ROWBYTES];

#define NEARZ 8                 /* Q1: 4 map units */
#define MAXCOORD 23000          /* Q1 deltas past this are not drawn */

/* view */
static s32 vx, vy;              /* Q1 */
static s16 viewz;               /* units */
static s16 vcos, vsin;          /* Q14 */

/* column clips: rows still open are ceilclip+1 .. floorclip-1 */
static s8 ceilclip[VIEWW], floorclip[VIEWW];

/* shades */
static u8 pat[SHADES][2];
static u8 shadelut[256];        /* brightness -> shade */
static u8 dimtab[64];           /* distance dimming by scale >> 4, of 256 */

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
static int clip_seg(s16 *x1, s16 *z1, s16 *x2, s16 *z2, u8 *c1, u8 *c2)
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
            *c1 = 1;
        } else {
            *x2 = (s16)(*x1 + (MUL16(*x2 - *x1, t) >> 14));
            *z2 = (s16)(*z1 + (MUL16(*z2 - *z1, t) >> 14));
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
    return (s16)(SCREENW * 8 + div32_16(MUL16(x, SCREENW * 8), z));
}

/* ---- solid ranges (Doom's solidsegs) ---- */
typedef struct { s16 first, last; } cliprange_t;
#define MAXSEGS 48
static cliprange_t solidsegs[MAXSEGS], *newend;

static void clear_clips(void)
{
    solidsegs[0].first = -0x7fff;
    solidsegs[0].last = -1;
    solidsegs[1].first = VIEWW;
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
} w;

static s32 yproj(s16 dh, s16 scale)
{
    s32 v = MUL16(dh, scale);           /* rows Q10 */
    if (v > 0x1000000) v = 0x1000000;
    if (v < -0x1000000) v = -0x1000000;
    return ((s32)CENTERY << 16) - (v << 6) + 0x8000;
}

/* value at the centre of column c, given its values at sx1 and sx2 */
static void interp(s32 a, s32 b, s32 *start, s32 *step, s16 c)
{
    s16 span = w.sx2 - w.sx1;
    s32 perq4;
    if (span < 1) span = 1;
    perq4 = ldiv32_16(b - a, span);
    *step = perq4 << 5;
    *start = a + perq4 * (s32)((s16)(c * 32 + 16) - w.sx1);
}

static u8 flat_shade(u8 tone, u8 light)
{
    return shadelut[(u8)(((u16)tone * light) >> 8) * 3 >> 2];
}

/* ---- column filling ---- */
static void vline(s16 x, s16 y1, s16 y2, u8 shade)
{
    u8 *p, keep, a, b;
    s16 n;
    if (y1 > y2) return;
    p = fb + y1 * ROWBYTES + (x >> 1);
    keep = (x & 1) ? 0xf0 : 0x0f;
    a = pat[shade][y1 & 1] & (u8)~keep;
    b = pat[shade][(y1 & 1) ^ 1] & (u8)~keep;
    n = y2 - y1 + 1;
    while (n >= 2) {
        p[0] = (p[0] & keep) | a;
        p[ROWBYTES] = (p[ROWBYTES] & keep) | b;
        p += 2 * ROWBYTES;
        n -= 2;
    }
    if (n) p[0] = (p[0] & keep) | a;
}

static void dot(s16 x, s16 y, u8 shade)
{
    u8 *p = fb + y * ROWBYTES + (x >> 1);
    u8 keep = (x & 1) ? 0xf0 : 0x0f;
    *p = (*p & keep) | (pat[shade][y & 1] & (u8)~keep);
}

#define EDGE 6

static void render_columns(s16 x, s16 stop)
{
    s32 top, bot, high = 0, low = 0, sc;
    s16 d = x - w.c0;

    /* values at the first column drawn */
    top = w.top + w.topstep * d;
    bot = w.bot + w.botstep * d;
    if (w.drawtop) high = w.high + w.highstep * d;
    if (w.drawbottom) low = w.low + w.lowstep * d;
    sc = w.sc + w.scstep * d;

    for (; x <= stop; x++) {
        s16 ctop = ceilclip[x] + 1, cbot = floorclip[x] - 1;
        s16 yl = (s16)(top >> 16), yh = (s16)(bot >> 16);
        s16 s = (s16)(sc >> 20);           /* scale / 16 */
        u8 dim = dimtab[s > 63 ? 63 : (s < 0 ? 0 : s)];
        u8 vedge = (x == w.c0 && w.edge1) || (x == w.c1 && w.edge2);
        u8 topedge = yl >= ctop, botedge = yh <= cbot;

        if (yl < ctop) yl = ctop;
        if (yh > cbot) yh = cbot;
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
                vline(x, yl, yh, vedge ? EDGE : shadelut[(w.bwall * dim) >> 8]);
                if (topedge) dot(x, yl, EDGE);
                if (botedge && yh > yl) dot(x, yh, EDGE);
            }
            ceilclip[x] = VIEWH;
            floorclip[x] = -1;
        } else {
            if (w.drawtop) {
                s16 mid = (s16)(high >> 16);
                high += w.highstep;
                if (mid >= cbot + 1) mid = cbot;
                if (mid >= yl) {
                    vline(x, yl, mid, vedge ? EDGE : shadelut[(w.btop * dim) >> 8]);
                    if (topedge) dot(x, yl, EDGE);
                    dot(x, mid, EDGE);
                    ceilclip[x] = (s8)mid;
                } else
                    ceilclip[x] = (s8)(yl - 1);
            } else if (w.markceil)
                ceilclip[x] = (s8)(yl - 1);
            if (w.drawbottom) {
                s16 mid = (s16)(low >> 16) + 1;
                low += w.lowstep;
                if (mid <= ceilclip[x]) mid = ceilclip[x] + 1;
                if (mid <= yh) {
                    vline(x, mid, yh, vedge ? EDGE : shadelut[(w.bbot * dim) >> 8]);
                    dot(x, mid, EDGE);
                    if (botedge && yh > mid) dot(x, yh, EDGE);
                    floorclip[x] = (s8)mid;
                } else
                    floorclip[x] = (s8)(yh + 1);
            } else if (w.markfloor)
                floorclip[x] = (s8)(yh + 1);
        }
        top += w.topstep;
        bot += w.botstep;
        sc += w.scstep;
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

static void add_seg(const seg_t *seg)
{
    s16 x1, z1, x2, z2, c0, c1;
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
    if (!clip_seg(&x1, &z1, &x2, &z2, &cut1, &cut2)) return;
    w.sx1 = project_x(x1, z1);
    w.sx2 = project_x(x2, z2);
    c0 = (w.sx1 + 31) >> 5;
    c1 = ((w.sx2 + 31) >> 5) - 1;
    if (c0 > c1) return;
    if (c0 < 0) c0 = 0;
    if (c1 > VIEWW - 1) c1 = VIEWW - 1;
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
        s16 s1 = div32_16(163840L, z1), s2 = div32_16(163840L, z2);
        s16 wt = front->ceilh - viewz, wb = front->floorh - viewz;
        interp(yproj(wt, s1), yproj(wt, s2), &w.top, &w.topstep, c0);
        interp(yproj(wb, s1), yproj(wb, s2), &w.bot, &w.botstep, c0);
        w.bot -= 0x10000;
        interp((s32)s1 << 16, (s32)s2 << 16, &w.sc, &w.scstep, c0);
        w.ceilshade = front->ceiltone ? flat_shade(front->ceiltone, front->light) : 1;
        w.floorshade = flat_shade(front->liquid ? front->floortone >> 1 : front->floortone,
                                  front->light);
        w.bwall = bright(seg->mid, front->light);
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
    s32 xfrac, xstep;           /* silhouette cell, 16.16, at x1 and per column */
    s32 ytop, ystep;            /* screen row of the top, 16.16; rows per cell */
    const u8 *sil;
    u16 clip;                   /* offset in the clip pool */
    u8 shadow;
} vissprite_t;

#define MAXVIS 48
#define POOL 2400
static vissprite_t vis[MAXVIS];
static s16 nvis;
static s8 poolc[POOL], poolf[POOL];
static u16 poolused;

static const u8 box_sil[] = { 4, 8, 4, 2, 8, 0,
    1, 0, 8, 1, 0, 8, 1, 0, 8, 1, 0, 8 };

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
    scale = div32_16(163840L, tz);
    /* left and right edges in Q1 units relative to the thing */
    left = (s32)tx - (s32)(s8)sil[3] * f * 2;
    right = left + (s32)cw * f * 2;
    if (left < -32000 || right > 32000) return;
    /* columns: x = 40 + x*40/z (low detail) */
    {
        s32 lx = ldiv32_16(MUL16((s16)left, SCREENW * 8), tz) + SCREENW * 8;
        s32 rx = ldiv32_16(MUL16((s16)right, SCREENW * 8), tz) + SCREENW * 8;
        x1 = (s16)((lx + 31) >> 5);
        x2 = (s16)(((rx + 31) >> 5) - 1);
        if (x1 > x2 || x2 < 0 || x1 >= VIEWW) return;
        v = &vis[nvis];
        /* cells per column */
        v->xstep = ldiv32_16((s32)cw << 16, (s16)((rx - lx) >> 5 ? (rx - lx) >> 5 : 1));
        v->xfrac = 0;
        if (x1 < 0) {
            v->xfrac = v->xstep * (s32)(-x1);
            x1 = 0;
        }
        if (x2 > VIEWW - 1) x2 = VIEWW - 1;
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

/* the runs of silhouette column cx: pointer to the count byte */
static const u8 *sil_column(const u8 *sil, s16 cx)
{
    const u8 *p = sil + 6;
    while (cx-- > 0) p += 1 + 2 * p[0];
    return p;
}

static void span(s16 x, s16 y1, s16 y2, s16 lo, s16 hi, u8 shade)
{
    if (y1 > y2) { s16 t = y1; y1 = y2; y2 = t; }
    if (y1 < lo) y1 = lo;
    if (y2 > hi) y2 = hi;
    vline(x, y1, y2, shade);
}

static void draw_sprite(vissprite_t *v)
{
    s16 x, cw = v->sil[0];
    s32 xf = v->xfrac;
    s16 ptop[8], pbot[8], pn = 0, lastcx = -1;
    const u8 *col = NULL;
    for (x = v->x1; x <= v->x2; x++, xf += v->xstep) {
        s16 cx = (s16)(xf >> 16), lo, hi, k, n;
        s16 tops[8], bots[8];
        if (cx >= cw) cx = cw - 1;
        if (cx != lastcx) {
            col = sil_column(v->sil, cx);
            lastcx = cx;
        }
        lo = poolc[v->clip + x - v->x1] + 1;
        hi = poolf[v->clip + x - v->x1] - 1;
        if (lo < 0) lo = 0;
        if (hi > VIEWH - 1) hi = VIEWH - 1;
        n = col[0];
        for (k = 0; k < n; k++) {
            tops[k] = (s16)((v->ytop + v->ystep * col[1 + 2 * k]) >> 16);
            bots[k] = (s16)((v->ytop + v->ystep * col[2 + 2 * k]) >> 16) - 1;
            if (bots[k] < tops[k]) bots[k] = tops[k];
        }
        if (lo <= hi) {
            for (k = 0; k < n; k++) {
                u8 whole = x == v->x1 || x == v->x2 || k >= pn;
                if (v->shadow) {
                    s16 y;
                    for (y = tops[k] & ~1; y <= bots[k]; y += 2)
                        if (y >= lo && y <= hi) dot(x, y, EDGE);
                    continue;
                }
                if (whole) {
                    span(x, tops[k], bots[k], lo, hi, EDGE);
                } else {
                    span(x, ptop[k], tops[k], lo, hi, EDGE);
                    span(x, pbot[k], bots[k], lo, hi, EDGE);
                }
            }
            /* a run that ends here: close it on the previous column's side */
        }
        for (k = 0; k < n; k++) {
            ptop[k] = tops[k];
            pbot[k] = bots[k];
        }
        pn = n;
    }
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
    if (!clip_seg(&x1, &z1, &x2, &z2, &cut1, &cut2)) return 0;
    c0 = (project_x(x1, z1) + 31) >> 5;
    c1 = ((project_x(x2, z2) + 31) >> 5) - 1;
    if (c0 > c1) {
        s16 t = c0; c0 = c1; c1 = t;
    }
    if (c0 < 0) c0 = 0;
    if (c1 > VIEWW - 1) c1 = VIEWW - 1;
    if (c0 > c1) return 0;
    start = solidsegs;
    while (start->last < c1) start++;
    if (c0 >= start->first && c1 <= start->last) return 0;
    return 1;
}

static void render_node(u16 bspnum)
{
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
        render_node(bsp->children[side]);
        if (solidsegs[0].last >= VIEWW - 1) return;      /* screen full */
        if (!check_bbox(bsp->bbox[side ^ 1])) return;
        bspnum = bsp->children[side ^ 1];
    }
    if (bspnum == 0xffff) bspnum = 0;
    render_subsector(bspnum & ~NF_SUBSECTOR);
}

/* ---- the player's weapon ---- */

static void draw_weapon(void)
{
    static const u8 wcls[8] = { C_W_FIST, C_W_PISTOL, C_W_SHOTGUN, C_W_CHAINGUN,
                                C_W_LAUNCHER, C_W_PLASMA, C_W_BFG, C_W_CHAINSAW };
    const u8 *sil = sprite_get(wcls[player.weapon & 7], player.weaponframe);
    s16 cw, ch, f, x, y0, bobx, boby;
    const u8 *col;
    if (!sil) sil = sprite_get(wcls[player.weapon & 7], 0);
    if (!sil || player.dead) return;
    cw = sil[0];
    ch = sil[1];
    f = sil[2];
    /* Doom places weapon sprites on a 320x200 screen with their offsets;
     * here a cell is f/2 columns... keep it simple: centre it at the bottom */
    bobx = (s16)(MUL16(fsin(gametic * 512), (s16)(player.bob >> 12)) >> 18);
    boby = (s16)((MUL16(fcos(gametic * 1024), (s16)(player.bob >> 12)) >> 18) & 7);
    x = VIEWW / 2 - (cw * f) / 8 + bobx;
    y0 = VIEWH - (ch * f) / 2 + 2 + boby;
    col = sil + 6;
    {
        s16 cx, k, px;
        s16 ptop[8], pbot[8], pn = 0;
        for (cx = 0; cx < cw; cx++) {
            s16 n = col[0];
            s16 ncols = (s16)(((cx + 1) * f) / 4 - (cx * f) / 4);
            for (px = 0; px < ncols; px++) {
                s16 sx = x + (cx * f) / 4 + px;
                if (sx < 0 || sx >= VIEWW) continue;
                for (k = 0; k < n; k++) {
                    s16 t = y0 + (col[1 + 2 * k] * f) / 2, b = y0 + (col[2 + 2 * k] * f) / 2 - 1;
                    if (b > VIEWH - 1) b = VIEWH - 1;
                    if (t > b) continue;
                    vline(sx, t, b, 2);
                    if (cx == 0 || cx == cw - 1 || k >= pn) {
                        dot(sx, t, EDGE);
                    } else {
                        span(sx, ptop[k], t, 0, VIEWH - 1, EDGE);
                        if (pbot[k] < VIEWH) span(sx, pbot[k], b, 0, VIEWH - 1, EDGE);
                    }
                    if (b < VIEWH - 1) dot(sx, b, EDGE);
                }
            }
            for (k = 0; k < n; k++) {
                ptop[k] = y0 + (col[1 + 2 * k] * f) / 2;
                pbot[k] = y0 + (col[2 + 2 * k] * f) / 2 - 1;
                if (pbot[k] > VIEWH - 1) pbot[k] = VIEWH;
            }
            pn = n;
            col += 1 + 2 * n;
        }
    }
}

/* ---- frame ---- */

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
    /* dimming by distance: scale/16 = 5120 / distance */
    for (i = 0; i < 64; i++) {
        s16 d = 112 + i * 8;
        dimtab[i] = (u8)(d > 255 ? 255 : d);
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
    for (i = 0; i < VIEWW; i++) {
        ceilclip[i] = -1;
        floorclip[i] = VIEWH;
    }
    clear_clips();
    nvis = 0;
    poolused = 0;
    if (lvl.nvertexes > MAXVERTS) return;
    render_node((u16)(lvl.nnodes - 1));
    for (i = nvis - 1; i >= 0; i--) draw_sprite(&vis[i]);
    draw_weapon();
}
