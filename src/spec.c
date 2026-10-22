/* Line and sector specials: doors, lifts, moving floors and ceilings,
 * stairs, teleporters, light changes, exits and damaging floors. The
 * subset of Doom's p_spec.c, p_doors.c, p_plats.c, p_floor.c,
 * p_ceilng.c, p_lights.c, p_telept.c and p_switch.c the Doom 1 maps use. */
#include "game.h"
#include "spec.h"

u8 exit_requested;

/* ---- surrounding sectors ---- */

static s16 sec_index(sector_t *s) { return (s16)(s - lvl.sectors); }

static s16 lowest_ceiling(sector_t *sec)
{
    s16 i, h = 0x7fff;
    for (i = 0; i < sec->nlines; i++) {
        sector_t *o = other_sector(&lvl.lines[sec->lines[i]], sec);
        if (o && o->ceilh < h) h = o->ceilh;
    }
    return h;
}

static s16 highest_ceiling(sector_t *sec)
{
    s16 i, h = -32000;
    for (i = 0; i < sec->nlines; i++) {
        sector_t *o = other_sector(&lvl.lines[sec->lines[i]], sec);
        if (o && o->ceilh > h) h = o->ceilh;
    }
    return h;
}

static s16 highest_floor(sector_t *sec)
{
    s16 i, h = -500;
    for (i = 0; i < sec->nlines; i++) {
        sector_t *o = other_sector(&lvl.lines[sec->lines[i]], sec);
        if (o && o->floorh > h) h = o->floorh;
    }
    return h;
}

static s16 lowest_floor(sector_t *sec)
{
    s16 i, h = sec->floorh;
    for (i = 0; i < sec->nlines; i++) {
        sector_t *o = other_sector(&lvl.lines[sec->lines[i]], sec);
        if (o && o->floorh < h) h = o->floorh;
    }
    return h;
}

static s16 next_highest_floor(sector_t *sec, s16 cur)
{
    s16 i, h = 0x7fff;
    for (i = 0; i < sec->nlines; i++) {
        sector_t *o = other_sector(&lvl.lines[sec->lines[i]], sec);
        if (o && o->floorh > cur && o->floorh < h) h = o->floorh;
    }
    return h == 0x7fff ? cur : h;
}

static u8 min_light(sector_t *sec, u8 max)
{
    s16 i;
    u8 m = max;
    for (i = 0; i < sec->nlines; i++) {
        sector_t *o = other_sector(&lvl.lines[sec->lines[i]], sec);
        if (o && o->light < m) m = o->light;
    }
    return m;
}

static u8 max_light(sector_t *sec)
{
    s16 i;
    u8 m = 0;
    for (i = 0; i < sec->nlines; i++) {
        sector_t *o = other_sector(&lvl.lines[sec->lines[i]], sec);
        if (o && o->light > m) m = o->light;
    }
    return m;
}

/* ---- moving sectors ---- */

enum { MV_DOOR = 1, MV_FLOOR, MV_PLAT, MV_CEIL, MV_LIGHT };
enum { DOOR_NORMAL, DOOR_CLOSE30, DOOR_CLOSE, DOOR_OPEN, DOOR_BLAZERAISE, DOOR_BLAZEOPEN,
       DOOR_BLAZECLOSE };
enum { FL_LOWER, FL_LOWEST, FL_TURBO, FL_RAISE, FL_NEAREST, FL_24, FL_CRUSH, FL_RAISETURBO,
       FL_512, FL_STAIRS8, FL_STAIRS16 };
enum { PL_DWUS, PL_BLAZEDWUS, PL_RAISENEAREST, PL_RAISE24, PL_RAISE32, PL_PERPETUAL };
enum { CL_LOWERTOFLOOR, CL_RAISETOHIGHEST, CL_LOWERANDCRUSH, CL_CRUSHANDRAISE, CL_FASTCRUSH };
enum { LT_FLICKER, LT_STROBEFAST, LT_STROBESLOW, LT_GLOW };

typedef struct {
    u8 type, kind;
    s8 dir;                     /* 1 up, -1 down, 0 waiting */
    u8 crush;
    sector_t *sec;
    s16 speed, dest, low, high;
    s16 wait, count;
    s8 olddir;
    u8 inactive;
} mover_t;

#define MAXMOVERS 96
static mover_t movers[MAXMOVERS];

void movers_clear(void)
{
    mset(movers, 0, sizeof movers);
}

static mover_t *new_mover(sector_t *sec, u8 type)
{
    s16 i;
    for (i = 0; i < MAXMOVERS; i++)
        if (!movers[i].type) {
            mover_t *m = &movers[i];
            mset(m, 0, sizeof *m);
            m->type = type;
            m->sec = sec;
            if (type != MV_LIGHT) sec->mover = m;
            return m;
        }
    return NULL;
}

static void end_mover(mover_t *m)
{
    if (m->sec->mover == m) m->sec->mover = NULL;
    m->type = 0;
}

/* after a sector moved: do the things in it still fit? */
static int change_sector(sector_t *sec, int crush)
{
    s16 i, si = sec_index(sec);
    int nofit = 0;
    for (i = 0; i < mobj_top; i++) {
        mobj_t *mo = &mobjs[i];
        if (!mo->inuse || !mo->ss || mo->ss->sector != si) continue;
        if (thing_height_clip(mo)) continue;
        if (mo->flags & MF_CORPSE) {
            mo->height = 0;
            continue;
        }
        if (!(mo->flags & MF_SHOOTABLE)) continue;
        nofit = 1;
        if (crush && !(gametic & 3)) damage_mobj(mo, NULL, NULL, 10);
    }
    return nofit;
}

enum { PL_OK, PL_CRUSHED, PL_DEST };

/* move the floor (floor = 1) or ceiling of sec toward dest */
static int move_plane(sector_t *sec, s16 speed, s16 dest, int crush, int floor, s8 dir)
{
    s16 *h = floor ? &sec->floorh : &sec->ceilh, last = *h;
    int reached = 0;
    if (dir < 0) {
        if (*h - speed <= dest) { *h = dest; reached = 1; }
        else *h -= speed;
    } else {
        if (*h + speed >= dest) { *h = dest; reached = 1; }
        else *h += speed;
    }
    if (floor && sec->floorh > sec->ceilh) sec->floorh = sec->ceilh;
    if (!floor && sec->ceilh < sec->floorh) sec->ceilh = sec->floorh;
    /* only moving into things can crush */
    if ((floor && dir > 0) || (!floor && dir < 0)) {
        if (change_sector(sec, crush)) {
            if (!crush) {
                *h = last;
                change_sector(sec, 0);
                return PL_CRUSHED;
            }
            if (!floor && crush) return PL_CRUSHED;
        }
    } else
        change_sector(sec, 0);
    return reached ? PL_DEST : PL_OK;
}

static void door_think(mover_t *d)
{
    int res;
    switch (d->dir) {
    case 0:
        if (--d->count) return;
        if (d->kind == DOOR_NORMAL || d->kind == DOOR_BLAZERAISE) d->dir = -1;
        else if (d->kind == DOOR_CLOSE30) d->dir = 1;
        return;
    case -1:
        res = move_plane(d->sec, d->speed, d->sec->floorh, 0, 0, -1);
        if (res == PL_DEST) {
            if (d->kind == DOOR_CLOSE30) {
                d->dir = 0;
                d->count = 30 * TICRATE;
            } else
                end_mover(d);
        } else if (res == PL_CRUSHED && d->kind != DOOR_CLOSE && d->kind != DOOR_BLAZECLOSE) {
            d->dir = 1;
        }
        return;
    case 1:
        res = move_plane(d->sec, d->speed, d->high, 0, 0, 1);
        if (res == PL_DEST) {
            if (d->kind == DOOR_NORMAL || d->kind == DOOR_BLAZERAISE) {
                d->dir = 0;
                d->count = 150;
            } else
                end_mover(d);
        }
        return;
    }
}

static void floor_think(mover_t *f)
{
    int res = move_plane(f->sec, f->speed, f->dest, f->crush, 1, f->dir);
    if (res == PL_DEST) end_mover(f);
}

static void plat_think(mover_t *p)
{
    int res;
    if (p->inactive) return;
    switch (p->dir) {
    case 1:
        res = move_plane(p->sec, p->speed, p->high, p->crush, 1, 1);
        if (res == PL_CRUSHED && !p->crush) {
            p->dir = -1;
        } else if (res == PL_DEST) {
            p->count = p->wait;
            p->dir = 0;
            p->olddir = 1;
            if (p->kind != PL_PERPETUAL) end_mover(p);
        }
        break;
    case -1:
        res = move_plane(p->sec, p->speed, p->low, 0, 1, -1);
        if (res == PL_DEST) {
            p->count = p->wait;
            p->dir = 0;
            p->olddir = -1;
        }
        break;
    case 0:
        if (!--p->count) p->dir = p->olddir == 1 ? -1 : 1;
        break;
    }
}

static void ceil_think(mover_t *c)
{
    int res;
    if (c->inactive) return;
    if (c->dir > 0) {
        res = move_plane(c->sec, c->speed, c->high, 0, 0, 1);
        if (res == PL_DEST) {
            if (c->kind == CL_CRUSHANDRAISE || c->kind == CL_FASTCRUSH) c->dir = -1;
            else end_mover(c);
        }
    } else {
        res = move_plane(c->sec, c->speed, c->low, c->crush, 0, -1);
        if (res == PL_DEST) {
            if (c->kind == CL_CRUSHANDRAISE || c->kind == CL_FASTCRUSH) c->dir = 1;
            else end_mover(c);
        } else if (res == PL_CRUSHED && c->kind == CL_CRUSHANDRAISE)
            c->speed = 1;
    }
}

static void light_think(mover_t *l)
{
    sector_t *s = l->sec;
    if (--l->count > 0) return;
    switch (l->kind) {
    case LT_FLICKER:
        if (s->light == (u8)l->high) {
            s->light = (u8)l->low;
            l->count = (prandom() & 7) + 1;
        } else {
            s->light = (u8)l->high;
            l->count = (prandom() & 63) + 1;
        }
        break;
    case LT_STROBEFAST:
    case LT_STROBESLOW:
        if (s->light == (u8)l->high) {
            s->light = (u8)l->low;
            l->count = l->kind == LT_STROBEFAST ? 15 : 35;
        } else {
            s->light = (u8)l->high;
            l->count = 5;
        }
        break;
    case LT_GLOW:
        if (l->dir < 0) {
            if (s->light <= l->low + 8) l->dir = 1;
            else s->light -= 8;
        } else {
            if (s->light >= l->high - 8) l->dir = -1;
            else s->light += 8;
        }
        l->count = 1;
        break;
    }
}

void movers_think(void)
{
    s16 i;
    for (i = 0; i < MAXMOVERS; i++) {
        mover_t *m = &movers[i];
        switch (m->type) {
        case MV_DOOR: door_think(m); break;
        case MV_FLOOR: floor_think(m); break;
        case MV_PLAT: plat_think(m); break;
        case MV_CEIL: ceil_think(m); break;
        case MV_LIGHT: light_think(m); break;
        }
    }
}

/* ---- starting movers ---- */

static void start_door(sector_t *sec, int kind)
{
    mover_t *d = new_mover(sec, MV_DOOR);
    if (!d) return;
    d->kind = (u8)kind;
    d->speed = kind >= DOOR_BLAZERAISE ? 8 : 2;
    d->high = lowest_ceiling(sec) - 4;
    switch (kind) {
    case DOOR_CLOSE:
    case DOOR_BLAZECLOSE:
        d->dir = -1;
        break;
    case DOOR_CLOSE30:
        d->dir = -1;
        break;
    default:
        d->dir = 1;
        if (d->high <= sec->ceilh && kind != DOOR_NORMAL && kind != DOOR_BLAZERAISE) {
            /* already open */
        }
    }
}

static int do_door_tag(s16 tag, int kind)
{
    s16 i;
    int rtn = 0;
    for (i = 0; i < lvl.nsectors; i++) {
        sector_t *s = &lvl.sectors[i];
        if (s->tag != tag || s->mover) continue;
        rtn = 1;
        start_door(s, kind);
    }
    return rtn;
}

static int has_key(int special)
{
    switch (special) {
    case 26: case 32: case 99: case 133:
        if (player.cards & (1 | 8)) return 1;
        set_message("YOU NEED A BLUE KEY");
        return 0;
    case 27: case 34: case 136: case 137:
        if (player.cards & (2 | 16)) return 1;
        set_message("YOU NEED A YELLOW KEY");
        return 0;
    case 28: case 33: case 134: case 135:
        if (player.cards & (4 | 32)) return 1;
        set_message("YOU NEED A RED KEY");
        return 0;
    }
    return 1;
}

/* a door on the line itself (D1/DR): the sector behind it */
static void vertical_door(line_t *line, mobj_t *thing)
{
    sector_t *sec;
    mover_t *d;
    int kind;
    if (line->sec[1] < 0) return;
    if (thing == player.mo && !has_key(line->special)) return;
    sec = &lvl.sectors[line->sec[1]];
    d = sec->mover;
    if (d && d->type == MV_DOOR) {
        switch (line->special) {
        case 1: case 26: case 27: case 28: case 117:
            if (d->dir == -1) d->dir = 1;
            else if (thing == player.mo) d->dir = -1;
            return;
        }
        return;
    }
    if (d) return;
    switch (line->special) {
    case 31: case 32: case 33: case 34: kind = DOOR_OPEN; break;
    case 117: kind = DOOR_BLAZERAISE; break;
    case 118: kind = DOOR_BLAZEOPEN; break;
    default: kind = DOOR_NORMAL;
    }
    start_door(sec, kind);
    if (kind == DOOR_OPEN || kind == DOOR_BLAZEOPEN) line->special = 0;
}

static int do_floor_tag(s16 tag, int kind)
{
    s16 i;
    int rtn = 0;
    for (i = 0; i < lvl.nsectors; i++) {
        sector_t *s = &lvl.sectors[i];
        mover_t *f;
        if (s->tag != tag || s->mover) continue;
        f = new_mover(s, MV_FLOOR);
        if (!f) break;
        rtn = 1;
        f->kind = (u8)kind;
        f->speed = 1;
        f->dir = 1;
        switch (kind) {
        case FL_LOWER: f->dir = -1; f->dest = highest_floor(s); break;
        case FL_LOWEST: f->dir = -1; f->dest = lowest_floor(s); break;
        case FL_TURBO:
            f->dir = -1;
            f->speed = 4;
            f->dest = highest_floor(s);
            if (f->dest != s->floorh) f->dest += 8;
            break;
        case FL_RAISE: case FL_CRUSH:
            f->dest = lowest_ceiling(s);
            if (f->dest > s->ceilh) f->dest = s->ceilh;
            if (kind == FL_CRUSH) {
                f->dest -= 8;
                f->crush = 1;
            }
            break;
        case FL_NEAREST: f->dest = next_highest_floor(s, s->floorh); break;
        case FL_RAISETURBO: f->speed = 4; f->dest = next_highest_floor(s, s->floorh); break;
        case FL_24: f->dest = s->floorh + 24; break;
        case FL_512: f->dest = s->floorh + 512; break;
        }
    }
    return rtn;
}

void ev_lower_floor_tag(s16 tag)
{
    do_floor_tag(tag, FL_LOWEST);
}

static int do_plat_tag(s16 tag, int kind, line_t *line)
{
    s16 i;
    int rtn = 0;
    /* reactivate stopped perpetual lifts */
    for (i = 0; i < MAXMOVERS; i++)
        if (movers[i].type == MV_PLAT && movers[i].inactive && movers[i].sec->tag == tag) {
            movers[i].inactive = 0;
            rtn = 1;
        }
    for (i = 0; i < lvl.nsectors; i++) {
        sector_t *s = &lvl.sectors[i];
        mover_t *p;
        if (s->tag != tag || s->mover) continue;
        p = new_mover(s, MV_PLAT);
        if (!p) break;
        rtn = 1;
        p->kind = (u8)kind;
        p->wait = 3 * TICRATE;
        switch (kind) {
        case PL_DWUS:
        case PL_BLAZEDWUS:
            p->speed = kind == PL_BLAZEDWUS ? 8 : 4;
            p->low = lowest_floor(s);
            if (p->low > s->floorh) p->low = s->floorh;
            p->high = s->floorh;
            p->dir = -1;
            break;
        case PL_RAISENEAREST:
            p->speed = 1;
            p->high = next_highest_floor(s, s->floorh);
            p->dir = 1;
            if (line && line->sec[0] >= 0) {
                s->floortone = lvl.sectors[line->sec[0]].floortone;
                s->floorflat = lvl.sectors[line->sec[0]].floorflat;
            }
            s->special = 0;
            break;
        case PL_RAISE24:
        case PL_RAISE32:
            p->speed = 1;
            p->high = s->floorh + (kind == PL_RAISE24 ? 24 : 32);
            p->dir = 1;
            if (line && line->sec[0] >= 0) {
                s->floortone = lvl.sectors[line->sec[0]].floortone;
                s->floorflat = lvl.sectors[line->sec[0]].floorflat;
            }
            break;
        case PL_PERPETUAL:
            p->speed = 4;
            p->low = lowest_floor(s);
            if (p->low > s->floorh) p->low = s->floorh;
            p->high = highest_floor(s);
            if (p->high < s->floorh) p->high = s->floorh;
            p->dir = (prandom() & 1) ? 1 : -1;
            break;
        }
    }
    return rtn;
}

static void stop_plat_tag(s16 tag)
{
    s16 i;
    for (i = 0; i < MAXMOVERS; i++)
        if (movers[i].type == MV_PLAT && movers[i].sec->tag == tag) movers[i].inactive = 1;
}

static int do_ceiling_tag(s16 tag, int kind)
{
    s16 i;
    int rtn = 0;
    for (i = 0; i < MAXMOVERS; i++)
        if (movers[i].type == MV_CEIL && movers[i].inactive && movers[i].sec->tag == tag) {
            movers[i].inactive = 0;
            rtn = 1;
        }
    for (i = 0; i < lvl.nsectors; i++) {
        sector_t *s = &lvl.sectors[i];
        mover_t *c;
        if (s->tag != tag || s->mover) continue;
        c = new_mover(s, MV_CEIL);
        if (!c) break;
        rtn = 1;
        c->kind = (u8)kind;
        c->speed = 1;
        c->high = s->ceilh;
        c->low = s->floorh;
        switch (kind) {
        case CL_LOWERTOFLOOR: c->dir = -1; break;
        case CL_RAISETOHIGHEST: c->dir = 1; c->high = highest_ceiling(s); break;
        case CL_LOWERANDCRUSH: c->dir = -1; c->low += 8; c->crush = 1; break;
        case CL_CRUSHANDRAISE: c->dir = -1; c->low += 8; c->crush = 1; break;
        case CL_FASTCRUSH: c->dir = -1; c->low += 8; c->crush = 1; c->speed = 2; break;
        }
    }
    return rtn;
}

static void stop_ceiling_tag(s16 tag)
{
    s16 i;
    for (i = 0; i < MAXMOVERS; i++)
        if (movers[i].type == MV_CEIL && movers[i].sec->tag == tag) movers[i].inactive = 1;
}

static int build_stairs(s16 tag, int turbo)
{
    s16 i;
    int rtn = 0;
    s16 stairsize = turbo ? 16 : 8, speed = turbo ? 4 : 1;
    for (i = 0; i < lvl.nsectors; i++) {
        sector_t *s = &lvl.sectors[i];
        mover_t *f;
        s16 height;
        int ok;
        if (s->tag != tag || s->mover) continue;
        f = new_mover(s, MV_FLOOR);
        if (!f) break;
        rtn = 1;
        height = s->floorh + stairsize;
        f->dir = 1;
        f->speed = speed;
        f->dest = height;
        do {
            s16 k;
            u8 tone = s->floortone;
            ok = 0;
            for (k = 0; k < s->nlines; k++) {
                line_t *l = &lvl.lines[s->lines[k]];
                sector_t *t;
                if (!(l->flags & ML_TWOSIDED) || l->sec[0] != sec_index(s) || l->sec[1] < 0) continue;
                t = &lvl.sectors[l->sec[1]];
                if (t->floortone != tone) continue;
                height += stairsize;
                if (t->mover) continue;
                s = t;
                f = new_mover(s, MV_FLOOR);
                if (!f) return rtn;
                f->dir = 1;
                f->speed = speed;
                f->dest = height;
                ok = 1;
                break;
            }
        } while (ok);
    }
    return rtn;
}

static void light_tag(s16 tag, int how)
{
    s16 i;
    for (i = 0; i < lvl.nsectors; i++) {
        sector_t *s = &lvl.sectors[i];
        if (s->tag != tag) continue;
        if (how < 0) s->light = max_light(s);
        else if (how == 0) s->light = min_light(s, s->light);
        else s->light = (u8)how;
    }
}

static int teleport(line_t *line, int side, mobj_t *thing)
{
    s16 i;
    if (thing->flags & MF_MISSILE) return 0;
    if (side == 1) return 0;
    for (i = 0; i < mobj_top; i++) {
        mobj_t *m = &mobjs[i];
        s32 oldx, oldy;
        if (!m->inuse || m->cls != C_TELEPORTMAN) continue;
        if (lvl.sectors[m->ss->sector].tag != line->tag) continue;
        oldx = thing->x;
        oldy = thing->y;
        /* telefrag whatever stands there */
        {
            s16 k;
            for (k = 0; k < mobj_top; k++) {
                mobj_t *o = &mobjs[k];
                s32 bd;
                if (!o->inuse || o == thing || !(o->flags & MF_SHOOTABLE)) continue;
                bd = ((s32)(o->radius + thing->radius)) << FRACBITS;
                if (ABS(o->x - m->x) >= bd || ABS(o->y - m->y) >= bd) continue;
                if (thing != player.mo && o == player.mo) return 0;
                damage_mobj(o, thing, thing, 10000);
            }
        }
        unset_position(thing);
        thing->x = m->x;
        thing->y = m->y;
        set_position(thing);
        check_position(thing, thing->x, thing->y);
        thing->floorz = tmfloorz;
        thing->ceilingz = tmceilingz;
        thing->z = thing->floorz;
        thing->angle = m->angle;
        thing->momx = thing->momy = thing->momz = 0;
        if (thing == player.mo) {
            thing->reactiontime = 18;
            player.viewz = thing->z + player.viewheight;
        }
        (void)oldx;
        (void)oldy;
        return 1;
    }
    return 0;
}

/* ---- the table ---- */

enum { T_W = 1, T_S, T_G, T_D };
enum { A_DOOR = 1, A_MDOOR, A_FLOOR, A_PLAT, A_PLATSTOP, A_CEIL, A_CEILSTOP, A_STAIRS,
       A_EXIT, A_TELE, A_LIGHT };

typedef struct {
    s16 special;
    u8 trig, repeat, action;
    s8 arg;
    u8 monsters;                /* monsters can trigger it */
} spdef_t;

static const spdef_t specials[] = {
    /* manual doors */
    {1, T_D, 1, A_MDOOR, 0, 1}, {26, T_D, 1, A_MDOOR, 0, 0}, {27, T_D, 1, A_MDOOR, 0, 0},
    {28, T_D, 1, A_MDOOR, 0, 0}, {31, T_D, 0, A_MDOOR, 0, 0}, {32, T_D, 0, A_MDOOR, 0, 0},
    {33, T_D, 0, A_MDOOR, 0, 0}, {34, T_D, 0, A_MDOOR, 0, 0}, {117, T_D, 1, A_MDOOR, 0, 0},
    {118, T_D, 0, A_MDOOR, 0, 0},
    /* tagged doors */
    {2, T_W, 0, A_DOOR, DOOR_OPEN, 0}, {3, T_W, 0, A_DOOR, DOOR_CLOSE, 0},
    {4, T_W, 0, A_DOOR, DOOR_NORMAL, 1}, {16, T_W, 0, A_DOOR, DOOR_CLOSE30, 0},
    {108, T_W, 0, A_DOOR, DOOR_BLAZERAISE, 0}, {109, T_W, 0, A_DOOR, DOOR_BLAZEOPEN, 0},
    {110, T_W, 0, A_DOOR, DOOR_BLAZECLOSE, 0},
    {75, T_W, 1, A_DOOR, DOOR_CLOSE, 0}, {76, T_W, 1, A_DOOR, DOOR_CLOSE30, 0},
    {86, T_W, 1, A_DOOR, DOOR_OPEN, 0}, {90, T_W, 1, A_DOOR, DOOR_NORMAL, 0},
    {105, T_W, 1, A_DOOR, DOOR_BLAZERAISE, 0}, {106, T_W, 1, A_DOOR, DOOR_BLAZEOPEN, 0},
    {107, T_W, 1, A_DOOR, DOOR_BLAZECLOSE, 0},
    {29, T_S, 0, A_DOOR, DOOR_NORMAL, 0}, {50, T_S, 0, A_DOOR, DOOR_CLOSE, 0},
    {103, T_S, 0, A_DOOR, DOOR_OPEN, 0}, {111, T_S, 0, A_DOOR, DOOR_BLAZERAISE, 0},
    {112, T_S, 0, A_DOOR, DOOR_BLAZEOPEN, 0}, {113, T_S, 0, A_DOOR, DOOR_BLAZECLOSE, 0},
    {133, T_S, 0, A_DOOR, DOOR_BLAZEOPEN, 0}, {135, T_S, 0, A_DOOR, DOOR_BLAZEOPEN, 0},
    {137, T_S, 0, A_DOOR, DOOR_BLAZEOPEN, 0},
    {42, T_S, 1, A_DOOR, DOOR_CLOSE, 0}, {61, T_S, 1, A_DOOR, DOOR_OPEN, 0},
    {63, T_S, 1, A_DOOR, DOOR_NORMAL, 0}, {114, T_S, 1, A_DOOR, DOOR_BLAZERAISE, 0},
    {115, T_S, 1, A_DOOR, DOOR_BLAZEOPEN, 0}, {116, T_S, 1, A_DOOR, DOOR_BLAZECLOSE, 0},
    {99, T_S, 1, A_DOOR, DOOR_BLAZEOPEN, 0}, {134, T_S, 1, A_DOOR, DOOR_BLAZEOPEN, 0},
    {136, T_S, 1, A_DOOR, DOOR_BLAZEOPEN, 0},
    {46, T_G, 1, A_DOOR, DOOR_OPEN, 0},
    /* floors */
    {5, T_W, 0, A_FLOOR, FL_RAISE, 0}, {19, T_W, 0, A_FLOOR, FL_LOWER, 0},
    {30, T_W, 0, A_FLOOR, FL_24, 0}, {36, T_W, 0, A_FLOOR, FL_TURBO, 0},
    {37, T_W, 0, A_FLOOR, FL_LOWEST, 0}, {38, T_W, 0, A_FLOOR, FL_LOWEST, 0},
    {56, T_W, 0, A_FLOOR, FL_CRUSH, 0}, {59, T_W, 0, A_FLOOR, FL_24, 0},
    {119, T_W, 0, A_FLOOR, FL_NEAREST, 0}, {130, T_W, 0, A_FLOOR, FL_RAISETURBO, 0},
    {82, T_W, 1, A_FLOOR, FL_LOWEST, 0}, {83, T_W, 1, A_FLOOR, FL_LOWER, 0},
    {84, T_W, 1, A_FLOOR, FL_LOWEST, 0}, {91, T_W, 1, A_FLOOR, FL_RAISE, 0},
    {92, T_W, 1, A_FLOOR, FL_24, 0}, {93, T_W, 1, A_FLOOR, FL_24, 0},
    {94, T_W, 1, A_FLOOR, FL_CRUSH, 0}, {96, T_W, 1, A_FLOOR, FL_24, 0},
    {98, T_W, 1, A_FLOOR, FL_TURBO, 0}, {128, T_W, 1, A_FLOOR, FL_NEAREST, 0},
    {129, T_W, 1, A_FLOOR, FL_RAISETURBO, 0},
    {18, T_S, 0, A_FLOOR, FL_NEAREST, 0}, {23, T_S, 0, A_FLOOR, FL_LOWEST, 0},
    {55, T_S, 0, A_FLOOR, FL_CRUSH, 0}, {71, T_S, 0, A_FLOOR, FL_TURBO, 0},
    {101, T_S, 0, A_FLOOR, FL_RAISE, 0}, {102, T_S, 0, A_FLOOR, FL_LOWER, 0},
    {131, T_S, 0, A_FLOOR, FL_RAISETURBO, 0}, {140, T_S, 0, A_FLOOR, FL_512, 0},
    {45, T_S, 1, A_FLOOR, FL_LOWER, 0}, {60, T_S, 1, A_FLOOR, FL_LOWEST, 0},
    {64, T_S, 1, A_FLOOR, FL_RAISE, 0}, {65, T_S, 1, A_FLOOR, FL_CRUSH, 0},
    {69, T_S, 1, A_FLOOR, FL_NEAREST, 0}, {70, T_S, 1, A_FLOOR, FL_TURBO, 0},
    {132, T_S, 1, A_FLOOR, FL_RAISETURBO, 0},
    {24, T_G, 0, A_FLOOR, FL_RAISE, 0},
    /* lifts */
    {10, T_W, 0, A_PLAT, PL_DWUS, 1}, {22, T_W, 0, A_PLAT, PL_RAISENEAREST, 0},
    {53, T_W, 0, A_PLAT, PL_PERPETUAL, 0}, {121, T_W, 0, A_PLAT, PL_BLAZEDWUS, 0},
    {88, T_W, 1, A_PLAT, PL_DWUS, 1}, {95, T_W, 1, A_PLAT, PL_RAISENEAREST, 0},
    {87, T_W, 1, A_PLAT, PL_PERPETUAL, 0}, {120, T_W, 1, A_PLAT, PL_BLAZEDWUS, 0},
    {21, T_S, 0, A_PLAT, PL_DWUS, 0}, {14, T_S, 0, A_PLAT, PL_RAISE32, 0},
    {15, T_S, 0, A_PLAT, PL_RAISE24, 0}, {20, T_S, 0, A_PLAT, PL_RAISENEAREST, 0},
    {122, T_S, 0, A_PLAT, PL_BLAZEDWUS, 0},
    {62, T_S, 1, A_PLAT, PL_DWUS, 0}, {66, T_S, 1, A_PLAT, PL_RAISE24, 0},
    {67, T_S, 1, A_PLAT, PL_RAISE32, 0}, {68, T_S, 1, A_PLAT, PL_RAISENEAREST, 0},
    {123, T_S, 1, A_PLAT, PL_BLAZEDWUS, 0},
    {54, T_W, 0, A_PLATSTOP, 0, 0}, {89, T_W, 1, A_PLATSTOP, 0, 0},
    {47, T_G, 0, A_PLAT, PL_RAISENEAREST, 0},
    /* ceilings */
    {40, T_W, 0, A_CEIL, CL_RAISETOHIGHEST, 0}, {44, T_W, 0, A_CEIL, CL_LOWERANDCRUSH, 0},
    {6, T_W, 0, A_CEIL, CL_FASTCRUSH, 0}, {25, T_W, 0, A_CEIL, CL_CRUSHANDRAISE, 0},
    {141, T_W, 0, A_CEIL, CL_CRUSHANDRAISE, 0},
    {72, T_W, 1, A_CEIL, CL_LOWERANDCRUSH, 0}, {73, T_W, 1, A_CEIL, CL_CRUSHANDRAISE, 0},
    {77, T_W, 1, A_CEIL, CL_FASTCRUSH, 0},
    {41, T_S, 0, A_CEIL, CL_LOWERTOFLOOR, 0}, {49, T_S, 0, A_CEIL, CL_CRUSHANDRAISE, 0},
    {43, T_S, 1, A_CEIL, CL_LOWERTOFLOOR, 0},
    {57, T_W, 0, A_CEILSTOP, 0, 0}, {74, T_W, 1, A_CEILSTOP, 0, 0},
    /* stairs */
    {8, T_W, 0, A_STAIRS, 0, 0}, {100, T_W, 0, A_STAIRS, 1, 0},
    {7, T_S, 0, A_STAIRS, 0, 0}, {127, T_S, 0, A_STAIRS, 1, 0},
    /* exits */
    {11, T_S, 0, A_EXIT, 1, 0}, {51, T_S, 0, A_EXIT, 2, 0},
    {52, T_W, 0, A_EXIT, 1, 0}, {124, T_W, 0, A_EXIT, 2, 0},
    /* teleporters */
    {39, T_W, 0, A_TELE, 0, 1}, {97, T_W, 1, A_TELE, 0, 1},
    {125, T_W, 0, A_TELE, 1, 1}, {126, T_W, 1, A_TELE, 1, 1},
    /* lights */
    {12, T_W, 0, A_LIGHT, -1, 0}, {13, T_W, 0, A_LIGHT, 127, 0}, {35, T_W, 0, A_LIGHT, 35, 0},
    {104, T_W, 0, A_LIGHT, 0, 0}, {79, T_W, 1, A_LIGHT, 35, 0}, {80, T_W, 1, A_LIGHT, -1, 0},
    {81, T_W, 1, A_LIGHT, 127, 0}, {138, T_S, 1, A_LIGHT, 127, 0},
    {139, T_S, 1, A_LIGHT, 35, 0},
};

static const spdef_t *find_special(s16 special)
{
    u16 i;
    for (i = 0; i < sizeof specials / sizeof specials[0]; i++)
        if (specials[i].special == special) return &specials[i];
    return NULL;
}

static int run_special(const spdef_t *d, line_t *line, int side, mobj_t *thing)
{
    switch (d->action) {
    case A_MDOOR: vertical_door(line, thing); return 1;
    case A_DOOR:
        if (thing == player.mo && !has_key(line->special)) return 0;
        return do_door_tag(line->tag, d->arg);
    case A_FLOOR: return do_floor_tag(line->tag, d->arg);
    case A_PLAT: return do_plat_tag(line->tag, d->arg, line);
    case A_PLATSTOP: stop_plat_tag(line->tag); return 1;
    case A_CEIL: return do_ceiling_tag(line->tag, d->arg);
    case A_CEILSTOP: stop_ceiling_tag(line->tag); return 1;
    case A_STAIRS: return build_stairs(line->tag, d->arg);
    case A_EXIT:
        if (thing != player.mo) return 0;
        exit_requested = (u8)d->arg;
        return 1;
    case A_TELE:
        if (d->arg && thing == player.mo) return 0;
        return teleport(line, side, thing);
    case A_LIGHT: light_tag(line->tag, d->arg == 127 ? 255 : d->arg); return 1;
    }
    return 0;
}

void cross_special_line(line_t *line, int side, mobj_t *thing)
{
    const spdef_t *d = find_special(line->special);
    if (!d || d->trig != T_W) return;
    if (thing != player.mo && (!d->monsters || (line->flags & ML_SECRET))) return;
    if (run_special(d, line, side, thing) && !d->repeat) line->special = 0;
}

void shoot_special_line(mobj_t *thing, line_t *line)
{
    const spdef_t *d = find_special(line->special);
    if (!d || d->trig != T_G || thing != player.mo) return;
    if (run_special(d, line, 0, thing) && !d->repeat) line->special = 0;
}

int use_special_line(mobj_t *thing, line_t *line, int side)
{
    const spdef_t *d = find_special(line->special);
    if (side && d && d->action != A_MDOOR) return 0;
    if (!d || (d->trig != T_S && d->trig != T_D)) return 0;
    if (thing != player.mo) {
        if (line->flags & ML_SECRET) return 0;
        if (!d->monsters) return 0;
    }
    if (run_special(d, line, side, thing) && !d->repeat) line->special = 0;
    return 1;
}

/* the player presses use: the first line within 64 units in front */
void use_lines(void)
{
    mobj_t *mo = player.mo;
    s32 px = mo->x >> FRACBITS, py = mo->y >> FRACBITS;
    s16 dx = (s16)(fcos(mo->angle) >> 8), dy = (s16)(fsin(mo->angle) >> 8);   /* 64 units */
    s16 bx0, bx1, by0, by1, bx, by;
    line_t *best = NULL;
    s32 bestt = 0x7fffffffL;
    s16 tries;
    /* walk the hits in order of distance: find the nearest, then the next */
    s32 after = -1;
    for (tries = 0; tries < 6; tries++) {
        best = NULL;
        bestt = 0x7fffffffL;
        bx0 = (s16)((MIN(px, px + dx) - lvl.bmorgx) >> 7);
        bx1 = (s16)((MAX(px, px + dx) - lvl.bmorgx) >> 7);
        by0 = (s16)((MIN(py, py + dy) - lvl.bmorgy) >> 7);
        by1 = (s16)((MAX(py, py + dy) - lvl.bmorgy) >> 7);
        validcount++;
        for (by = MAX(by0, 0); by <= by1 && by < lvl.bmheight; by++)
            for (bx = MAX(bx0, 0); bx <= bx1 && bx < lvl.bmwidth; bx++) {
                const s16 *list = lvl.blockmap + lvl.blockmap[4 + by * lvl.bmwidth + bx];
                for (list++; *list != -1; list++) {
                    line_t *l = &lvl.lines[*list];
                    vertex_t *a;
                    s32 ax, ay, den, nt, nu, t;
                    if (l->validcount == validcount) continue;
                    l->validcount = validcount;
                    a = &lvl.vertexes[l->v1];
                    ax = a->x - px;
                    ay = a->y - py;
                    if (ax > 30000 || ax < -30000 || ay > 30000 || ay < -30000) continue;
                    den = MUL16(dx, l->dy) - MUL16(dy, l->dx);
                    if (!den) continue;
                    nt = MUL16(ax, l->dy) - MUL16(ay, l->dx);
                    nu = MUL16(ax, dy) - MUL16(ay, dx);
                    if (den < 0) { den = -den; nt = -nt; nu = -nu; }
                    if (nt < 0 || nt > den || nu < 0 || nu > den) continue;
                    t = fixdiv(nt, den);
                    if (t <= after || t >= bestt) continue;
                    best = l;
                    bestt = t;
                }
            }
        if (!best) return;
        if (best->special) {
            int side = point_on_line_side(mo->x, mo->y, best);
            use_special_line(mo, best, side);
            return;
        }
        if (best->sec[1] < 0 || !(best->flags & ML_TWOSIDED)) return;   /* oof */
        {
            sector_t *f = &lvl.sectors[best->sec[0]], *b = &lvl.sectors[best->sec[1]];
            if (MIN(f->ceilh, b->ceilh) <= MAX(f->floorh, b->floorh)) return;
        }
        after = bestt;
    }
}

/* ---- sectors ---- */

void player_in_special_sector(void)
{
    mobj_t *mo = player.mo;
    sector_t *sec = &lvl.sectors[mo->ss->sector];
    if (mo->z != mo->floorz) return;
    switch (sec->special) {
    case 5:
        if (!player.powers[3] && !(gametic & 31)) damage_mobj(mo, NULL, NULL, 10);
        break;
    case 7:
        if (!player.powers[3] && !(gametic & 31)) damage_mobj(mo, NULL, NULL, 5);
        break;
    case 16:
    case 4:
        if ((!player.powers[3] || prandom() < 5) && !(gametic & 31)) damage_mobj(mo, NULL, NULL, 20);
        break;
    case 9:
        player.secrets++;
        sec->special = 0;
        set_message("A SECRET IS REVEALED!");
        break;
    case 11:
        player.cheat = 0;
        if (!(gametic & 31)) damage_mobj(mo, NULL, NULL, 20);
        if (player.health <= 10) exit_requested = 1;
        break;
    }
}

/* lights that flicker or glow, from the sector specials */
void spawn_specials(void)
{
    s16 i;
    totalsecrets = 0;
    for (i = 0; i < lvl.nsectors; i++) {
        sector_t *s = &lvl.sectors[i];
        mover_t *l;
        int kind = -1;
        switch (s->special) {
        case 1: kind = LT_FLICKER; break;
        case 2: case 4: kind = LT_STROBEFAST; break;
        case 3: kind = LT_STROBESLOW; break;
        case 8: kind = LT_GLOW; break;
        case 12: kind = LT_STROBESLOW; break;
        case 13: kind = LT_STROBEFAST; break;
        case 17: kind = LT_FLICKER; break;
        case 9: totalsecrets++; break;
        }
        if (kind < 0) continue;
        l = new_mover(s, MV_LIGHT);
        if (!l) break;
        l->kind = (u8)kind;
        l->high = s->light;
        l->low = min_light(s, s->light);
        if (l->low == l->high) l->low = 0;
        l->count = (prandom() & 7) + 1;
        l->dir = -1;
    }
}
