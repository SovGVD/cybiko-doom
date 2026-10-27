/* Things: spawning, linking, movement and collision, line of sight,
 * attacks and damage. A cut-down version of Doom's p_mobj.c, p_map.c,
 * p_maputl.c, p_sight.c and p_inter.c. */
#include "game.h"
#include "spec.h"
#include "sound.h"

mobj_t mobjs[MAXMOBJS];
static s16 mobj_hint;
s16 mobj_top;

/* ---- linking ---- */

static s16 block_of(s32 x, s32 y)
{
    s16 bx = (s16)(((x >> FRACBITS) - lvl.bmorgx) >> 7);
    s16 by = (s16)(((y >> FRACBITS) - lvl.bmorgy) >> 7);
    if (bx < 0 || by < 0 || bx >= lvl.bmwidth || by >= lvl.bmheight) return -1;
    return by * lvl.bmwidth + bx;
}

void set_position(mobj_t *mo)
{
    subsector_t *ss = point_in_subsector(mo->x, mo->y);
    mo->ss = ss;
    mo->snext = ss->things;
    ss->things = mo;
    mo->bcell = block_of(mo->x, mo->y);
    if (mo->bcell >= 0) {
        mo->bnext = lvl.blocklinks[mo->bcell];
        lvl.blocklinks[mo->bcell] = mo;
    }
}

void unset_position(mobj_t *mo)
{
    mobj_t **p;
    if (mo->ss) {
        for (p = &mo->ss->things; *p; p = &(*p)->snext)
            if (*p == mo) {
                *p = mo->snext;
                break;
            }
        mo->ss = NULL;
    }
    if (mo->bcell >= 0) {
        for (p = &lvl.blocklinks[mo->bcell]; *p; p = &(*p)->bnext)
            if (*p == mo) {
                *p = mo->bnext;
                break;
            }
        mo->bcell = -1;
    }
}

mobj_t *spawn_mobj(s32 x, s32 y, s32 z, int cls)
{
    s16 i;
    mobj_t *mo = NULL;
    const info_t *in = &info[cls];
    for (i = 0; i < MAXMOBJS; i++) {
        s16 k = (s16)((mobj_hint + i) & (MAXMOBJS - 1));
        if (!mobjs[k].inuse) {
            mo = &mobjs[k];
            mobj_hint = k + 1;
            if (k >= mobj_top) mobj_top = k + 1;
            break;
        }
    }
    if (!mo) return NULL;
    mset(mo, 0, sizeof *mo);
    mo->inuse = 1;
    mo->x = x;
    mo->y = y;
    mo->cls = (s16)cls;
    mo->radius = in->radius;
    mo->height = in->height;
    mo->health = in->health;
    mo->flags = in->flags;
    mo->reactiontime = in->reaction;
    mo->bcell = -1;
    set_position(mo);
    {
        sector_t *s = &lvl.sectors[mo->ss->sector];
        mo->floorz = (s32)s->floorh << FRACBITS;
        mo->ceilingz = (s32)s->ceilh << FRACBITS;
    }
    if (z == ONFLOORZ) mo->z = mo->floorz;
    else if (z == ONCEILINGZ) mo->z = mo->ceilingz - ((s32)mo->height << FRACBITS);
    else mo->z = z;
    return mo;
}

void remove_mobj(mobj_t *mo)
{
    unset_position(mo);
    mo->inuse = 0;
}

/* ---- collision ---- */

static mobj_t *tmthing;
static s32 tmx, tmy, tmbox[4];
s32 tmfloorz, tmceilingz, tmdropoffz;
line_t *ceilingline;
line_t *spechit[16];
s16 numspechit;
mobj_t *blockthing;     /* the thing that stopped a move */
line_t *blockline;      /* the line that stopped a move */

static int check_line(line_t *ld)
{
    sector_t *front, *back;
    s32 opentop, openbottom, lowfloor;
    if (tmbox[BOXRIGHT] <= ((s32)ld->bbox[BOXLEFT] << FRACBITS)
        || tmbox[BOXLEFT] >= ((s32)ld->bbox[BOXRIGHT] << FRACBITS)
        || tmbox[BOXTOP] <= ((s32)ld->bbox[BOXBOTTOM] << FRACBITS)
        || tmbox[BOXBOTTOM] >= ((s32)ld->bbox[BOXTOP] << FRACBITS))
        return 1;
    if (box_on_line_side(tmbox, ld) != -1) return 1;
    if (ld->sec[1] < 0 || !(ld->flags & ML_TWOSIDED)) {
        blockline = ld;
        return 0;
    }
    if (!(tmthing->flags & MF_MISSILE)) {
        if (ld->flags & ML_BLOCKING) {
            blockline = ld;
            return 0;
        }
        if ((ld->flags & ML_BLOCKMONSTERS) && tmthing != player.mo) {
            blockline = ld;
            return 0;
        }
    }
    front = &lvl.sectors[ld->sec[0]];
    back = &lvl.sectors[ld->sec[1]];
    opentop = (s32)MIN(front->ceilh, back->ceilh) << FRACBITS;
    openbottom = (s32)MAX(front->floorh, back->floorh) << FRACBITS;
    lowfloor = (s32)MIN(front->floorh, back->floorh) << FRACBITS;
    if (opentop < tmceilingz) {
        tmceilingz = opentop;
        ceilingline = ld;
    }
    if (openbottom > tmfloorz) tmfloorz = openbottom;
    if (lowfloor < tmdropoffz) tmdropoffz = lowfloor;
    if (ld->special && numspechit < 16) spechit[numspechit++] = ld;
    return 1;
}

static int check_thing(mobj_t *th)
{
    s32 blockdist;
    if (!(th->flags & (MF_SOLID | MF_PICKUP | MF_SHOOTABLE)) || th == tmthing) return 1;
    blockdist = ((s32)(th->radius + tmthing->radius)) << FRACBITS;
    if (ABS(th->x - tmx) >= blockdist || ABS(th->y - tmy) >= blockdist) return 1;
    if (tmthing->flags & MF_MISSILE) {
        if (tmthing->z > th->z + ((s32)th->height << FRACBITS)) return 1;
        if (tmthing->z + ((s32)tmthing->height << FRACBITS) < th->z) return 1;
        if (th == tmthing->target) return 1;            /* own missile */
        if (!(th->flags & MF_SHOOTABLE)) return !(th->flags & MF_SOLID);
        blockthing = th;
        return 0;
    }
    if (tmthing->flags & MF_SKULLFLY) {
        if (th->flags & MF_SHOOTABLE) {
            blockthing = th;
            return 0;
        }
    }
    if (th->flags & MF_PICKUP) {
        if (tmthing == player.mo) touch_special(th);
        return 1;
    }
    if (!(th->flags & MF_SOLID)) return 1;
    blockthing = th;
    return 0;
}

/* can tmthing be at x, y? sets tmfloorz, tmceilingz, spechit */
int check_position(mobj_t *mo, s32 x, s32 y)
{
    s16 xl, xh, yl, yh, bx, by;
    s32 r = (s32)mo->radius << FRACBITS;
    sector_t *s;
    tmthing = mo;
    tmx = x;
    tmy = y;
    tmbox[BOXTOP] = y + r;
    tmbox[BOXBOTTOM] = y - r;
    tmbox[BOXRIGHT] = x + r;
    tmbox[BOXLEFT] = x - r;
    s = &lvl.sectors[point_in_subsector(x, y)->sector];
    tmfloorz = tmdropoffz = (s32)s->floorh << FRACBITS;
    tmceilingz = (s32)s->ceilh << FRACBITS;
    ceilingline = NULL;
    blockthing = NULL;
    blockline = NULL;
    numspechit = 0;
    validcount++;

    xl = (s16)(((tmbox[BOXLEFT] >> FRACBITS) - lvl.bmorgx - 32) >> 7);
    xh = (s16)(((tmbox[BOXRIGHT] >> FRACBITS) - lvl.bmorgx + 32) >> 7);
    yl = (s16)(((tmbox[BOXBOTTOM] >> FRACBITS) - lvl.bmorgy - 32) >> 7);
    yh = (s16)(((tmbox[BOXTOP] >> FRACBITS) - lvl.bmorgy + 32) >> 7);
    if (xl < 0) xl = 0;
    if (yl < 0) yl = 0;
    if (xh >= lvl.bmwidth) xh = lvl.bmwidth - 1;
    if (yh >= lvl.bmheight) yh = lvl.bmheight - 1;
    for (by = yl; by <= yh; by++)
        for (bx = xl; bx <= xh; bx++) {
            mobj_t *th;
            for (th = lvl.blocklinks[by * lvl.bmwidth + bx]; th; th = th->bnext)
                if (!check_thing(th)) return 0;
        }
    xl = (s16)(((tmbox[BOXLEFT] >> FRACBITS) - lvl.bmorgx) >> 7);
    xh = (s16)(((tmbox[BOXRIGHT] >> FRACBITS) - lvl.bmorgx) >> 7);
    yl = (s16)(((tmbox[BOXBOTTOM] >> FRACBITS) - lvl.bmorgy) >> 7);
    yh = (s16)(((tmbox[BOXTOP] >> FRACBITS) - lvl.bmorgy) >> 7);
    if (xl < 0) xl = 0;
    if (yl < 0) yl = 0;
    if (xh >= lvl.bmwidth) xh = lvl.bmwidth - 1;
    if (yh >= lvl.bmheight) yh = lvl.bmheight - 1;
    for (by = yl; by <= yh; by++)
        for (bx = xl; bx <= xh; bx++) {
            const s16 *list = lvl.blockmap + lvl.blockmap[4 + by * lvl.bmwidth + bx];
            for (list++; *list != -1; list++) {
                line_t *ld = &lvl.lines[*list];
                if (ld->validcount == validcount) continue;
                ld->validcount = validcount;
                if (!check_line(ld)) return 0;
            }
        }
    return 1;
}

int try_move(mobj_t *mo, s32 x, s32 y)
{
    s32 oldx, oldy;
    s16 i;
    if (!check_position(mo, x, y)) return 0;
    if (!(mo->flags & MF_NOGRAVITY) || (mo->flags & MF_FLOAT) || 1) {
        if (tmceilingz - tmfloorz < ((s32)mo->height << FRACBITS)) return 0;
        if (!(mo->flags & MF_MISSILE) && !(mo->flags & MF_FLOAT)) {
            if (tmceilingz - mo->z < ((s32)mo->height << FRACBITS)) return 0;
            if (tmfloorz - mo->z > 24 * FRACUNIT) return 0;
            if (mo != player.mo && tmfloorz - tmdropoffz > 24 * FRACUNIT) return 0;
        } else if (mo->flags & MF_FLOAT) {
            if (tmceilingz - mo->z < ((s32)mo->height << FRACBITS)) return 0;
            if (tmfloorz > mo->z) return 0;
        }
    }
    unset_position(mo);
    oldx = mo->x;
    oldy = mo->y;
    mo->floorz = tmfloorz;
    mo->ceilingz = tmceilingz;
    mo->x = x;
    mo->y = y;
    set_position(mo);
    if (!(mo->flags & MF_MISSILE))
        for (i = numspechit - 1; i >= 0; i--) {
            line_t *ld = spechit[i];
            int side = point_on_line_side(x, y, ld), oldside = point_on_line_side(oldx, oldy, ld);
            if (side != oldside) cross_special_line(ld, oldside, mo);
        }
    return 1;
}

/* recompute floorz/ceilingz after a sector moved; 0 if it no longer fits */
int thing_height_clip(mobj_t *mo)
{
    int onfloor = mo->z == mo->floorz;
    check_position(mo, mo->x, mo->y);
    mo->floorz = tmfloorz;
    mo->ceilingz = tmceilingz;
    if (onfloor || mo->z < mo->floorz) mo->z = mo->floorz;
    else if (mo->z + ((s32)mo->height << FRACBITS) > mo->ceilingz)
        mo->z = mo->ceilingz - ((s32)mo->height << FRACBITS);
    return mo->ceilingz - mo->floorz >= ((s32)mo->height << FRACBITS);
}

/* ---- movement ---- */

#define FRICTION 0xe800
#define STOPSPEED 0x1000
#define GRAVITY FRACUNIT
#define MAXMOVE (30 * FRACUNIT)

static s32 mulfrac(s32 a, u16 f)
{
    int neg = a < 0;
    u32 ua = (u32)(neg ? -a : a);
    u32 r = (ua >> 16) * f + (((ua & 0xffff) * f) >> 16);
    return neg ? -(s32)r : (s32)r;
}

static void explode_missile(mobj_t *mo)
{
    mo->momx = mo->momy = mo->momz = 0;
    mo->state = S_EXPLODE;
    mo->frame = 1;
    mo->tics = 8;
    mo->flags &= ~MF_MISSILE;
    sound_at(mo, mo->cls == C_ROCKETSHOT ? SFX_BAREXP : SFX_FIRXPL);
    if (mo->cls == C_ROCKETSHOT) radius_attack(mo, mo->target, 128);
}

static void missile_hit(mobj_t *mo)
{
    if (blockthing) {
        s16 dmg = (s16)(((prandom() & 7) + 1) * info[mo->cls].melee);
        damage_mobj(blockthing, mo, mo->target, dmg);
    }
    explode_missile(mo);
}

static void xy_movement(mobj_t *mo)
{
    s32 xmove, ymove;
    if (!mo->momx && !mo->momy) return;
    if (mo->momx > MAXMOVE) mo->momx = MAXMOVE;
    else if (mo->momx < -MAXMOVE) mo->momx = -MAXMOVE;
    if (mo->momy > MAXMOVE) mo->momy = MAXMOVE;
    else if (mo->momy < -MAXMOVE) mo->momy = -MAXMOVE;
    xmove = mo->momx;
    ymove = mo->momy;
    do {
        s32 px, py;
        if (xmove > MAXMOVE / 2 || ymove > MAXMOVE / 2 || xmove < -MAXMOVE / 2 || ymove < -MAXMOVE / 2) {
            px = mo->x + xmove / 2;
            py = mo->y + ymove / 2;
            xmove >>= 1;
            ymove >>= 1;
        } else {
            px = mo->x + xmove;
            py = mo->y + ymove;
            xmove = ymove = 0;
        }
        if (!try_move(mo, px, py)) {
            if (mo->flags & MF_MISSILE) {
                if (!blockthing && ceilingline && ceilingline->sec[1] >= 0
                    && !lvl.sectors[ceilingline->sec[1]].ceiltone) {
                    remove_mobj(mo);             /* into the sky */
                    return;
                }
                missile_hit(mo);
                return;
            }
            if (mo->flags & MF_SKULLFLY) {
                if (blockthing) damage_mobj(blockthing, mo, mo, (s16)(((prandom() & 7) + 1) * 3));
                mo->flags &= ~MF_SKULLFLY;
                mo->momx = mo->momy = mo->momz = 0;
                mo->state = S_CHASE;
                return;
            }
            /* slide along the wall: try each axis on its own */
            if (try_move(mo, mo->x, py)) mo->momx = 0;
            else if (try_move(mo, px, mo->y)) mo->momy = 0;
            else mo->momx = mo->momy = 0;
            xmove = ymove = 0;
        }
    } while (xmove || ymove);

    if (mo->flags & (MF_MISSILE | MF_SKULLFLY)) return;
    if (mo->z > mo->floorz) return;               /* no friction in the air */
    if (mo->flags & MF_CORPSE) {
        mo->momx = mo->momy = 0;
        return;
    }
    if (mo->momx > -STOPSPEED && mo->momx < STOPSPEED && mo->momy > -STOPSPEED
        && mo->momy < STOPSPEED) {
        mo->momx = mo->momy = 0;
    } else {
        mo->momx = mulfrac(mo->momx, FRICTION);
        mo->momy = mulfrac(mo->momy, FRICTION);
    }
}

static void z_movement(mobj_t *mo)
{
    s32 h = (s32)mo->height << FRACBITS;
    if (mo == player.mo && mo->z < mo->floorz) {
        player.viewheight -= mo->floorz - mo->z;
        player.deltaviewheight = ((41 * FRACUNIT) - player.viewheight) >> 3;
    }
    mo->z += mo->momz;
    if ((mo->flags & MF_FLOAT) && mo->target && !(mo->flags & MF_SKULLFLY) && mo->state != S_DEAD
        && mo->state != S_DYING) {
        s32 delta = mo->target->z + (h >> 1) - mo->z;
        u16 dist = approx_dist((mo->x - mo->target->x) >> 16, (mo->y - mo->target->y) >> 16);
        if (delta < 0 && dist < (u16)(-(delta >> 16)) * 3) mo->z -= 4 * FRACUNIT;
        else if (delta > 0 && dist < (u16)(delta >> 16) * 3) mo->z += 4 * FRACUNIT;
    }
    if (mo->z <= mo->floorz) {
        if (mo->flags & MF_SKULLFLY) mo->momz = -mo->momz;
        if (mo->momz < 0) {
            if (mo == player.mo && mo->momz < -GRAVITY * 8)
                player.deltaviewheight = mo->momz >> 3;
            mo->momz = 0;
        }
        mo->z = mo->floorz;
        if ((mo->flags & MF_MISSILE) && mo->state != S_EXPLODE) {
            explode_missile(mo);
            return;
        }
    } else if (!(mo->flags & MF_NOGRAVITY)) {
        if (!mo->momz) mo->momz = -GRAVITY * 2;
        else mo->momz -= GRAVITY;
    }
    if (mo->z + h > mo->ceilingz) {
        if (mo->momz > 0) mo->momz = 0;
        mo->z = mo->ceilingz - h;
        if (mo->flags & MF_SKULLFLY) mo->momz = -mo->momz;
        if ((mo->flags & MF_MISSILE) && mo->state != S_EXPLODE) explode_missile(mo);
    }
}

void mobj_physics(mobj_t *mo)
{
    if (mo->momx || mo->momy || (mo->flags & MF_SKULLFLY)) {
        xy_movement(mo);
        if (!mo->inuse) return;
    }
    if (mo->z != mo->floorz || mo->momz) z_movement(mo);
}

/* ---- sight (Doom p_sight.c, without the slope tests) ---- */

static s32 sx1, sy1, sx2, sy2;   /* units */
static s16 sdx, sdy;

static int divline_side(s32 x, s32 y, s32 lx, s32 ly, s32 ldx, s32 ldy)
{
    s32 dx = x - lx, dy = y - ly;
    while (dx > 32767 || dx < -32767 || dy > 32767 || dy < -32767 || ldx > 32767
           || ldx < -32767 || ldy > 32767 || ldy < -32767) {
        dx >>= 1; dy >>= 1; ldx >>= 1; ldy >>= 1;
    }
    return MUL16(dy, ldx) >= MUL16(ldy, dx);
}

static int cross_subsector(u16 num)
{
    subsector_t *ss = &lvl.subsectors[num];
    seg_t *seg = &lvl.segs[ss->firstseg];
    s16 n = ss->numsegs;
    for (; n--; seg++) {
        line_t *l = &lvl.lines[seg->line];
        vertex_t *v1, *v2;
        sector_t *f, *b;
        if (l->validcount == validcount) continue;
        l->validcount = validcount;
        v1 = &lvl.vertexes[l->v1];
        v2 = &lvl.vertexes[l->v2];
        if (divline_side(v1->x, v1->y, sx1, sy1, sdx, sdy) == divline_side(v2->x, v2->y, sx1, sy1, sdx, sdy))
            continue;
        if (divline_side(sx1, sy1, v1->x, v1->y, l->dx, l->dy) == divline_side(sx2, sy2, v1->x, v1->y, l->dx, l->dy))
            continue;
        if (l->sec[1] < 0 || !(l->flags & ML_TWOSIDED)) return 0;
        f = &lvl.sectors[l->sec[0]];
        b = &lvl.sectors[l->sec[1]];
        if (MIN(f->ceilh, b->ceilh) <= MAX(f->floorh, b->floorh)) return 0;
    }
    return 1;
}

/* Doom P_CrossBSPNode without the recursion (the on-chip stack is small):
 * the nodes still to look at wait on their own stack. Every subsector the
 * line crosses must let it through, so the order does not matter. */
#define SIGHTSTACK 96
static u16 sightstack[SIGHTSTACK];

static int cross_bsp(void)
{
    u16 sp = 1;
    sightstack[0] = (u16)(lvl.nnodes - 1);
    while (sp) {
        u16 bspnum = sightstack[--sp];
        while (!(bspnum & NF_SUBSECTOR)) {
            node_t *bsp = &lvl.nodes[bspnum];
            int side = divline_side(sx1, sy1, bsp->x, bsp->y, bsp->dx, bsp->dy) ^ 1;
            if (side != (divline_side(sx2, sy2, bsp->x, bsp->y, bsp->dx, bsp->dy) ^ 1)) {
                if (sp == SIGHTSTACK) return 0;
                sightstack[sp++] = bsp->children[side ^ 1];
            }
            bspnum = bsp->children[side];
        }
        if (!cross_subsector(bspnum == 0xffff ? 0 : bspnum & ~NF_SUBSECTOR)) return 0;
    }
    return 1;
}

int check_sight(mobj_t *a, mobj_t *b)
{
    s16 s1 = a->ss->sector, s2 = b->ss->sector;
    s32 pnum = (s32)s1 * lvl.nsectors + s2;
    if (lvl.nreject && (pnum >> 3) < (u16)lvl.nreject && (lvl.reject[pnum >> 3] & (1 << (pnum & 7))))
        return 0;
    validcount++;
    sx1 = a->x >> FRACBITS;
    sy1 = a->y >> FRACBITS;
    sx2 = b->x >> FRACBITS;
    sy2 = b->y >> FRACBITS;
    sdx = (s16)(sx2 - sx1);
    sdy = (s16)(sy2 - sy1);
    if (sx2 - sx1 > 32767 || sx2 - sx1 < -32767 || sy2 - sy1 > 32767 || sy2 - sy1 < -32767) return 0;
    if (!lvl.nnodes) return 1;
    return cross_bsp();
}

/* ---- damage ---- */

void kill_mobj(mobj_t *source, mobj_t *target)
{
    target->flags &= ~(MF_SHOOTABLE | MF_FLOAT | MF_SKULLFLY | MF_SOLID);
    target->flags |= MF_CORPSE;
    if (target->cls != C_LOSTSOUL) target->flags &= ~MF_NOGRAVITY;
    target->state = S_DYING;
    target->frame = FR_PAIN;
    target->tics = 10;
    target->height >>= 2;
    if (target->flags & MF_COUNTKILL) player.kills++;
    if (target == player.mo) {
        player.dead = 1;
        player.health = 0;
        sound_start(SFX_PLDETH);
        return;
    }
    sound_say(target, SAY_DEATH);
    if (target->cls == C_BARREL) {
        target->frame = FR_ATTACK;
        target->state = S_EXPLODE;
        target->tics = 10;
        radius_attack(target, source, 128);
        return;
    }
    if (info[target->cls].drop) {
        mobj_t *mo = spawn_mobj(target->x, target->y, ONFLOORZ, info[target->cls].drop);
        if (mo) mo->flags |= MF_DROPPED;
    }
    boss_death(target);
}

void damage_mobj(mobj_t *target, mobj_t *inflictor, mobj_t *source, s16 damage)
{
    if (!(target->flags & MF_SHOOTABLE) || target->health <= 0) return;
    if (target->flags & MF_SKULLFLY) target->momx = target->momy = target->momz = 0;
    if (inflictor && target != player.mo && !(target->flags & MF_NOGRAVITY) && damage < 100
        && info[target->cls].mass) {
        /* push the target away a little */
        angle_t a = point_to_angle(target->x - inflictor->x, target->y - inflictor->y);
        s32 thrust = (s32)damage * (FRACUNIT >> 3) * 100 / info[target->cls].mass;
        target->momx += (thrust >> 14) * fcos(a);
        target->momy += (thrust >> 14) * fsin(a);
    }
    if (target == player.mo) {
        if (player.cheat || player.powers[0]) return;
        if (skill == 0) damage >>= 1;
        if (player.armortype) {
            s16 saved = player.armortype == 1 ? damage / 3 : damage / 2;
            if (player.armor <= saved) {
                saved = player.armor;
                player.armortype = 0;
            }
            player.armor -= saved;
            damage -= saved;
        }
        player.health -= damage;
        if (player.health < 0) player.health = 0;
        player.damagecount += damage;
        if (player.damagecount > 100) player.damagecount = 100;
        if (player.health > 0) sound_start(SFX_PLPAIN);
    }
    target->health -= damage;
    if (target->health <= 0) {
        kill_mobj(source, target);
        return;
    }
    if (target != player.mo && prandom() < info[target->cls].painchance
        && !(target->flags & MF_SKULLFLY)) {
        target->flags |= MF_JUSTHIT;
        sound_say(target, SAY_PAIN);
        target->state = S_PAIN;
        target->frame = FR_PAIN;
        target->tics = 6;
    }
    target->reactiontime = 0;
    if (source && source != target && target != player.mo
        && (!target->threshold || target->cls == C_ZOMBIE) && source->cls != target->cls
        && (source->flags & MF_SHOOTABLE)) {
        target->target = source;
        target->threshold = 100;
        if (target->state == S_IDLE) target->state = S_CHASE;
    }
}

void radius_attack(mobj_t *spot, mobj_t *source, s16 damage)
{
    s16 i;
    for (i = 0; i < mobj_top; i++) {
        mobj_t *th = &mobjs[i];
        s32 dx, dy;
        s16 dist;
        if (!th->inuse || !(th->flags & MF_SHOOTABLE)) continue;
        dx = ABS(th->x - spot->x) >> FRACBITS;
        dy = ABS(th->y - spot->y) >> FRACBITS;
        if (dx > damage + 40 || dy > damage + 40) continue;
        dist = (s16)MAX(dx, dy) - th->radius;
        if (dist < 0) dist = 0;
        if (dist >= damage) continue;
        if (th == spot || check_sight(th, spot)) damage_mobj(th, spot, source, damage - dist);
    }
}

/* ---- hitscan attacks ---- */

mobj_t *linetarget;

/* the nearest shootable thing within range on the line at angle a that the
 * shooter can see; with slop it also takes things a little off the line
 * (Doom's autoaim) */
mobj_t *aim_line(mobj_t *t1, angle_t a, s16 range, s16 slop)
{
    s16 i, bestz = 0x7fff;
    s16 c = fcos(a), s = fsin(a);
    mobj_t *best = NULL;
    for (i = 0; i < mobj_top; i++) {
        mobj_t *th = &mobjs[i];
        s32 dx, dy;
        s16 tz, tx;
        if (!th->inuse || th == t1 || !(th->flags & MF_SHOOTABLE)) continue;
        dx = (th->x - t1->x) >> FRACBITS;
        dy = (th->y - t1->y) >> FRACBITS;
        if (dx > range || dx < -range || dy > range || dy < -range) continue;
        tz = (s16)((MUL16(dx, c) + MUL16(dy, s)) >> 14);
        if (tz <= 0 || tz > range || tz >= bestz) continue;
        tx = (s16)((MUL16(dx, s) - MUL16(dy, c)) >> 14);
        if (ABS(tx) > th->radius + slop + (tz >> 4)) continue;
        if (!check_sight(t1, th)) continue;
        best = th;
        bestz = tz;
    }
    linetarget = best;
    return best;
}

void spawn_puff(s32 x, s32 y, s32 z)
{
    mobj_t *mo = spawn_mobj(x, y, z, C_PUFF);
    if (mo) {
        mo->state = S_EXPLODE;
        mo->tics = 6;
        mo->momz = FRACUNIT;
    }
}

/* fire a bullet at angle a; returns the thing hit */
mobj_t *line_attack(mobj_t *t1, angle_t a, s16 range, s16 damage)
{
    mobj_t *th = aim_line(t1, a, range, 0);
    if (th) {
        damage_mobj(th, t1, t1, damage);
        if (th->inuse && !(th->flags & MF_NOGRAVITY) && th->cls != C_BARREL)
            spawn_puff(th->x - ((s32)fcos(a) << 3), th->y - ((s32)fsin(a) << 3), th->z + ((s32)th->height << 15));
        else if (th->inuse)
            spawn_puff(th->x - ((s32)fcos(a) << 3), th->y - ((s32)fsin(a) << 3), th->z + ((s32)th->height << 15));
    }
    return th;
}

mobj_t *spawn_missile(mobj_t *source, mobj_t *dest, int cls, angle_t an)
{
    mobj_t *th = spawn_mobj(source->x, source->y, source->z + 32 * FRACUNIT, cls);
    s16 speed = info[cls].speed;
    u16 dist;
    if (!th) return NULL;
    th->target = source;
    th->angle = an;
    th->momx = (s32)speed * fcos(an) << 2;
    th->momy = (s32)speed * fsin(an) << 2;
    if (dest) {
        dist = approx_dist((dest->x - source->x) >> FRACBITS, (dest->y - source->y) >> FRACBITS) / speed;
        if (dist < 1) dist = 1;
        th->momz = ((dest->z - source->z) + (16 * FRACUNIT) - (8 * FRACUNIT)) / (s32)dist;
    }
    th->state = S_FLY;
    th->frame = 0;
    /* move a little so it starts outside the shooter */
    if (!try_move(th, th->x + (th->momx >> 1), th->y + (th->momy >> 1))) {
        if (th->inuse) missile_hit(th);
    }
    return th;
}
