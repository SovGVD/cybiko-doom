/* Loading a map from the level pack, and the geometry queries on it. */
#include "level.h"
#include "game.h"

level_t lvl;
u16 validcount = 1;
const u8 *pack;                 /* set by the platform code */

#define ARENA_SIZE (200L * 1024)
static u8 arena[ARENA_SIZE];
static u32 arena_used;

static void *alloc(u32 n)
{
    void *p;
    n = (n + 3) & ~3UL;
    if (arena_used + n > ARENA_SIZE) return NULL;
    p = arena + arena_used;
    arena_used += n;
    mset(p, 0, n);
    return p;
}

static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 rd32(const u8 *p) { return (u32)rd16(p) << 16 | rd16(p + 2); }

int level_count(void) { return rd16(pack + 6); }

static const u8 *map_data(int i) { return pack + rd32(pack + 12 + 4 * i); }

const char *level_name(int i)
{
    static char name[5];
    const u8 *m = map_data(i);
    int k;
    for (k = 0; k < 4; k++) name[k] = (char)m[k];
    name[4] = 0;
    return name;
}

int level_find(const char *name)
{
    int i, k;
    for (i = 0; i < level_count(); i++) {
        const char *n = level_name(i);
        for (k = 0; k < 4 && n[k] == name[k]; k++) ;
        if (k == 4) return i;
    }
    return -1;
}

const u8 *sprite_get(int cls, int frame)
{
    int slot = cls * 5 + frame, maps = level_count();
    u32 off;
    if (slot >= rd16(pack + 8)) return NULL;
    off = rd32(pack + 12 + 4 * (maps + slot));
    return off ? pack + off : NULL;
}

/* a map part: count and data */
static const u8 *part(const u8 *m, int k, s16 *count)
{
    *count = (s16)rd16(m + 4 + 6 * k);
    return m + rd32(m + 4 + 6 * k + 2);
}

int level_load(int idx)
{
    const u8 *m = map_data(idx), *p;
    s16 i, n;
    int k;

    arena_used = 0;
    mset(&lvl, 0, sizeof lvl);
    for (k = 0; k < 4; k++) lvl.name[k] = (char)m[k];

    p = part(m, 0, &n);
    lvl.nvertexes = n;
    lvl.vertexes = alloc((u32)n * sizeof(vertex_t));
    for (i = 0; i < n; i++, p += 4) {
        lvl.vertexes[i].x = (s16)rd16(p);
        lvl.vertexes[i].y = (s16)rd16(p + 2);
    }

    p = part(m, 1, &n);
    lvl.nsectors = n;
    lvl.sectors = alloc((u32)n * sizeof(sector_t));
    for (i = 0; i < n; i++, p += 12) {
        sector_t *s = &lvl.sectors[i];
        s->floorh = s->orig_floor = (s16)rd16(p);
        s->ceilh = s->orig_ceil = (s16)rd16(p + 2);
        s->light = p[4];
        s->special = p[5];
        s->tag = (s16)rd16(p + 6);
        s->floortone = p[8];
        s->ceiltone = p[9];
        s->liquid = p[10];
    }

    p = part(m, 2, &n);
    lvl.nsides = n;
    lvl.sides = alloc((u32)n * sizeof(side_t));
    for (i = 0; i < n; i++, p += 6) {
        side_t *s = &lvl.sides[i];
        s->top = p[0];
        s->bottom = p[1];
        s->mid = p[2];
        s->sector = (s16)rd16(p + 4);
    }

    p = part(m, 3, &n);
    lvl.nlines = n;
    lvl.lines = alloc((u32)n * sizeof(line_t));
    for (i = 0; i < n; i++, p += 14) {
        line_t *l = &lvl.lines[i];
        vertex_t *a, *b;
        l->v1 = (s16)rd16(p);
        l->v2 = (s16)rd16(p + 2);
        l->flags = rd16(p + 4);
        l->special = (s16)rd16(p + 6);
        l->tag = (s16)rd16(p + 8);
        l->side[0] = (s16)rd16(p + 10);
        l->side[1] = (s16)rd16(p + 12);
        l->sec[0] = l->side[0] >= 0 ? lvl.sides[l->side[0]].sector : -1;
        l->sec[1] = l->side[1] >= 0 ? lvl.sides[l->side[1]].sector : -1;
        a = &lvl.vertexes[l->v1];
        b = &lvl.vertexes[l->v2];
        l->dx = b->x - a->x;
        l->dy = b->y - a->y;
        l->bbox[BOXTOP] = MAX(a->y, b->y);
        l->bbox[BOXBOTTOM] = MIN(a->y, b->y);
        l->bbox[BOXLEFT] = MIN(a->x, b->x);
        l->bbox[BOXRIGHT] = MAX(a->x, b->x);
    }

    p = part(m, 4, &n);
    lvl.nsegs = n;
    lvl.segs = alloc((u32)n * sizeof(seg_t));
    for (i = 0; i < n; i++, p += 8) {
        seg_t *s = &lvl.segs[i];
        line_t *l;
        side_t *sd;
        u16 fl;
        s->v1 = (s16)rd16(p);
        s->v2 = (s16)rd16(p + 2);
        s->line = (s16)rd16(p + 4);
        fl = rd16(p + 6);
        s->side = fl & 1;
        s->edge = (u8)(fl >> 1 & 3);
        l = &lvl.lines[s->line];
        s->front = l->sec[s->side];
        s->back = (l->flags & ML_TWOSIDED) ? l->sec[s->side ^ 1] : -1;
        sd = &lvl.sides[l->side[s->side]];
        s->top = sd->top;
        s->bottom = sd->bottom;
        s->mid = sd->mid;
    }

    p = part(m, 5, &n);
    lvl.nsubsectors = n;
    lvl.subsectors = alloc((u32)n * sizeof(subsector_t));
    for (i = 0; i < n; i++, p += 6) {
        subsector_t *s = &lvl.subsectors[i];
        s->numsegs = (s16)rd16(p);
        s->firstseg = (s16)rd16(p + 2);
        s->sector = (s16)rd16(p + 4);
    }

    p = part(m, 6, &n);
    lvl.nnodes = n;
    lvl.nodes = alloc((u32)n * sizeof(node_t));
    for (i = 0; i < n; i++, p += 28) {
        node_t *d = &lvl.nodes[i];
        int j;
        d->x = (s16)rd16(p);
        d->y = (s16)rd16(p + 2);
        d->dx = (s16)rd16(p + 4);
        d->dy = (s16)rd16(p + 6);
        for (j = 0; j < 4; j++) {
            d->bbox[0][j] = (s16)rd16(p + 8 + 2 * j);
            d->bbox[1][j] = (s16)rd16(p + 16 + 2 * j);
        }
        d->children[0] = rd16(p + 24);
        d->children[1] = rd16(p + 26);
    }

    p = part(m, 7, &n);
    lvl.nthings = n;
    lvl.things = alloc((u32)n * sizeof(mapthing_t));
    for (i = 0; i < n; i++, p += 10) {
        mapthing_t *t = &lvl.things[i];
        t->x = (s16)rd16(p);
        t->y = (s16)rd16(p + 2);
        t->angle = (s16)rd16(p + 4);
        t->cls = (s16)rd16(p + 6);
        t->flags = (s16)rd16(p + 8);
    }

    p = part(m, 8, &n);
    lvl.blockmap = alloc((u32)(u16)n * 2);
    for (i = 0; i < n; i++, p += 2) lvl.blockmap[i] = (s16)rd16(p);
    lvl.bmorgx = lvl.blockmap[0];
    lvl.bmorgy = lvl.blockmap[1];
    lvl.bmwidth = lvl.blockmap[2];
    lvl.bmheight = lvl.blockmap[3];
    lvl.blocklinks = alloc((u32)lvl.bmwidth * (u32)lvl.bmheight * sizeof(void *));

    p = part(m, 9, &n);
    lvl.reject = p;
    lvl.nreject = n;

    /* lines per sector */
    {
        s16 *cnt = alloc((u32)lvl.nsectors * 2);
        for (i = 0; i < lvl.nlines; i++) {
            line_t *l = &lvl.lines[i];
            if (l->sec[0] >= 0) cnt[l->sec[0]]++;
            if (l->sec[1] >= 0 && l->sec[1] != l->sec[0]) cnt[l->sec[1]]++;
        }
        for (i = 0; i < lvl.nsectors; i++) {
            lvl.sectors[i].lines = alloc((u32)cnt[i] * 2);
            if (!lvl.sectors[i].lines && cnt[i]) return 0;
        }
        for (i = 0; i < lvl.nlines; i++) {
            line_t *l = &lvl.lines[i];
            sector_t *s;
            if (l->sec[0] >= 0) {
                s = &lvl.sectors[l->sec[0]];
                s->lines[s->nlines++] = i;
            }
            if (l->sec[1] >= 0 && l->sec[1] != l->sec[0]) {
                s = &lvl.sectors[l->sec[1]];
                s->lines[s->nlines++] = i;
            }
        }
    }
    return lvl.blocklinks != NULL;
}

/* ---- geometry ---- */

/* Sign of the cross product of (dx, dy) (16.16, relative to the line's
 * start) with the line's direction (ldx, ldy, map units): 1 = back side.
 * Uses 1/16-unit precision near the line, whole units far from it, so it
 * stays within 16 x 16 multiplies. */
static int cross_side(s32 dx, s32 dy, s16 ldx, s16 ldy)
{
    s32 a, b;
    dx >>= 12;
    dy >>= 12;
    if (dx < -32767 || dx > 32767 || dy < -32767 || dy > 32767) {
        dx >>= 4;
        dy >>= 4;
    }
    a = MUL16(dx, ldy);
    b = MUL16(dy, ldx);
    return b >= a;
}

static int node_side(s32 x, s32 y, const node_t *n)
{
    if (!n->dx) {
        if (x <= ((s32)n->x << FRACBITS)) return n->dy > 0;
        return n->dy < 0;
    }
    if (!n->dy) {
        if (y <= ((s32)n->y << FRACBITS)) return n->dx < 0;
        return n->dx > 0;
    }
    return cross_side(x - ((s32)n->x << FRACBITS), y - ((s32)n->y << FRACBITS), n->dx, n->dy);
}

subsector_t *point_in_subsector(s32 x, s32 y)
{
    u16 n;
    if (!lvl.nnodes) return &lvl.subsectors[0];
    n = (u16)(lvl.nnodes - 1);
    while (!(n & NF_SUBSECTOR)) n = lvl.nodes[n].children[node_side(x, y, &lvl.nodes[n])];
    return &lvl.subsectors[n & ~NF_SUBSECTOR];
}

int point_on_line_side(s32 x, s32 y, const line_t *l)
{
    const vertex_t *v = &lvl.vertexes[l->v1];
    if (!l->dx) {
        if (x <= ((s32)v->x << FRACBITS)) return l->dy > 0;
        return l->dy < 0;
    }
    if (!l->dy) {
        if (y <= ((s32)v->y << FRACBITS)) return l->dx < 0;
        return l->dx > 0;
    }
    return cross_side(x - ((s32)v->x << FRACBITS), y - ((s32)v->y << FRACBITS), l->dx, l->dy);
}

/* -1 when the box (16.16, Doom order) crosses the line */
int box_on_line_side(const s32 *box, const line_t *l)
{
    int p1, p2;
    const vertex_t *v = &lvl.vertexes[l->v1];
    if (!l->dy) {
        s32 y = (s32)v->y << FRACBITS;
        p1 = box[BOXTOP] > y;
        p2 = box[BOXBOTTOM] > y;
        if (l->dx < 0) { p1 ^= 1; p2 ^= 1; }
    } else if (!l->dx) {
        s32 x = (s32)v->x << FRACBITS;
        p1 = box[BOXRIGHT] < x;
        p2 = box[BOXLEFT] < x;
        if (l->dy < 0) { p1 ^= 1; p2 ^= 1; }
    } else if ((l->dy > 0) == (l->dx > 0)) {
        p1 = point_on_line_side(box[BOXLEFT], box[BOXTOP], l);
        p2 = point_on_line_side(box[BOXRIGHT], box[BOXBOTTOM], l);
    } else {
        p1 = point_on_line_side(box[BOXRIGHT], box[BOXTOP], l);
        p2 = point_on_line_side(box[BOXLEFT], box[BOXBOTTOM], l);
    }
    return p1 == p2 ? p1 : -1;
}

sector_t *other_sector(const line_t *l, const sector_t *s)
{
    if (!(l->flags & ML_TWOSIDED) || l->sec[1] < 0) return NULL;
    if (&lvl.sectors[l->sec[0]] == s) return &lvl.sectors[l->sec[1]];
    return &lvl.sectors[l->sec[0]];
}
