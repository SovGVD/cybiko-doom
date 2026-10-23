/* Monster behaviour, after Doom's p_enemy.c: look for the player, chase
 * with the eight-direction walk, melee, hitscan and missile attacks. */
#include "game.h"
#include "spec.h"

enum { DI_EAST, DI_NE, DI_NORTH, DI_NW, DI_WEST, DI_SW, DI_SOUTH, DI_SE, DI_NODIR };
static const s32 xspeed[8] = { FRACUNIT, 47000, 0, -47000, -FRACUNIT, -47000, 0, 47000 };
static const s32 yspeed[8] = { 0, 47000, FRACUNIT, 47000, 0, -47000, -FRACUNIT, -47000 };

#define MELEERANGE 64

void ev_lower_floor_tag(s16 tag);

/* ---- hearing ---- */

/* Doom P_RecursiveSound as a breadth-first flood (the on-chip stack is
 * small): first every sector the sound reaches through open lines, then
 * those behind one sound-blocking line. soundtraversed 1 or 2 says which. */
#define SOUNDQ 256
static u16 soundq[2][SOUNDQ];

static void sound_flood(sector_t *start, mobj_t *target)
{
    u16 n[2], head, level;
    n[0] = 1;
    n[1] = 0;
    soundq[0][0] = (u16)(start - lvl.sectors);
    start->validcount = validcount;
    start->soundtraversed = 1;
    for (level = 0; level < 2; level++) {
        for (head = 0; head < n[level]; head++) {
            sector_t *sec = &lvl.sectors[soundq[level][head]];
            s16 i;
            if (sec->soundtraversed != level + 1) continue;   /* reached better since */
            sec->soundtarget = target;
            for (i = 0; i < sec->nlines; i++) {
                line_t *l = &lvl.lines[sec->lines[i]];
                sector_t *other, *f, *b;
                u16 to;
                if (!(l->flags & ML_TWOSIDED) || l->sec[1] < 0) continue;
                f = &lvl.sectors[l->sec[0]];
                b = &lvl.sectors[l->sec[1]];
                if (MIN(f->ceilh, b->ceilh) <= MAX(f->floorh, b->floorh)) continue;   /* closed door */
                other = f == sec ? b : f;
                to = (l->flags & ML_SOUNDBLOCK) ? level + 1 : level;
                if (to > 1) continue;
                if (other->validcount == validcount && other->soundtraversed <= to + 1) continue;
                if (n[to] == SOUNDQ) continue;
                other->validcount = validcount;
                other->soundtraversed = (u8)(to + 1);
                soundq[to][n[to]++] = (u16)(other - lvl.sectors);
            }
        }
    }
}

void noise_alert(mobj_t *target, mobj_t *emitter)
{
    validcount++;
    sound_flood(&lvl.sectors[emitter->ss->sector], target);
}

/* ---- looking and walking ---- */

static u16 dist_to(mobj_t *a, mobj_t *b)
{
    return approx_dist((b->x - a->x) >> FRACBITS, (b->y - a->y) >> FRACBITS);
}

static void face_target(mobj_t *mo)
{
    if (!mo->target) return;
    mo->angle = point_to_angle(mo->target->x - mo->x, mo->target->y - mo->y);
    if (mo->target->flags & MF_SHADOW) mo->angle += (angle_t)((s16)(prandom() - prandom()) << 5);
}

static int look_for_player(mobj_t *mo, int allaround)
{
    mobj_t *pm = player.mo;
    if (player.dead || !pm) return 0;
    if (!check_sight(mo, pm)) return 0;
    if (!allaround) {
        angle_t an = (angle_t)(point_to_angle(pm->x - mo->x, pm->y - mo->y) - mo->angle);
        if (an > ANG90 && an < (angle_t)(0 - ANG90) && dist_to(mo, pm) > MELEERANGE) return 0;
    }
    mo->target = pm;
    return 1;
}

static int do_move(mobj_t *mo)
{
    s32 tryx, tryy;
    s16 speed = info[mo->cls].speed;
    if (mo->movedir == DI_NODIR) return 0;
    tryx = mo->x + speed * xspeed[mo->movedir];
    tryy = mo->y + speed * yspeed[mo->movedir];
    if (!try_move(mo, tryx, tryy)) {
        s16 i;
        int good = 0;
        if ((mo->flags & MF_FLOAT) && !blockthing && !blockline) {
            if (mo->z < tmfloorz) mo->z += 4 * FRACUNIT;
            else mo->z -= 4 * FRACUNIT;
            return 1;
        }
        for (i = numspechit - 1; i >= 0; i--)
            if (use_special_line(mo, spechit[i], 0)) good = 1;
        return good;
    }
    if (!(mo->flags & MF_FLOAT)) mo->z = mo->floorz;
    return 1;
}

static int try_walk(mobj_t *mo)
{
    if (!do_move(mo)) return 0;
    mo->movecount = prandom() & 15;
    return 1;
}

static void new_chase_dir(mobj_t *mo)
{
    s32 dx, dy;
    u8 d1, d2, olddir = mo->movedir, turnaround, tdir;
    static const u8 diags[4] = { DI_NW, DI_NE, DI_SW, DI_SE };
    if (!mo->target) return;
    turnaround = olddir == DI_NODIR ? DI_NODIR : (u8)((olddir + 4) & 7);
    dx = mo->target->x - mo->x;
    dy = mo->target->y - mo->y;
    d1 = dx > 10 * FRACUNIT ? DI_EAST : dx < -10 * FRACUNIT ? DI_WEST : DI_NODIR;
    d2 = dy < -10 * FRACUNIT ? DI_SOUTH : dy > 10 * FRACUNIT ? DI_NORTH : DI_NODIR;
    if (d1 != DI_NODIR && d2 != DI_NODIR) {
        mo->movedir = diags[((dy < 0) << 1) + (dx > 0)];
        if (mo->movedir != turnaround && try_walk(mo)) return;
    }
    if (prandom() > 200 || ABS(dy) > ABS(dx)) {
        tdir = d1;
        d1 = d2;
        d2 = tdir;
    }
    if (d1 == turnaround) d1 = DI_NODIR;
    if (d2 == turnaround) d2 = DI_NODIR;
    if (d1 != DI_NODIR) {
        mo->movedir = d1;
        if (try_walk(mo)) return;
    }
    if (d2 != DI_NODIR) {
        mo->movedir = d2;
        if (try_walk(mo)) return;
    }
    if (olddir != DI_NODIR) {
        mo->movedir = olddir;
        if (try_walk(mo)) return;
    }
    if (prandom() & 1) {
        for (tdir = DI_EAST; tdir <= DI_SE; tdir++)
            if (tdir != turnaround) {
                mo->movedir = tdir;
                if (try_walk(mo)) return;
            }
    } else {
        for (tdir = DI_SE + 1; tdir-- > DI_EAST;)
            if (tdir != turnaround) {
                mo->movedir = tdir;
                if (try_walk(mo)) return;
            }
    }
    if (turnaround != DI_NODIR) {
        mo->movedir = turnaround;
        if (try_walk(mo)) return;
    }
    mo->movedir = DI_NODIR;
}

static int check_melee(mobj_t *mo)
{
    if (!mo->target || !info[mo->cls].melee) return 0;
    if (dist_to(mo, mo->target) >= MELEERANGE - 20 + mo->target->radius) return 0;
    return check_sight(mo, mo->target);
}

static int check_missile(mobj_t *mo)
{
    s16 dist;
    const info_t *in = &info[mo->cls];
    if (!in->missile && !in->shots && mo->cls != C_LOSTSOUL) return 0;
    if (!check_sight(mo, mo->target)) return 0;
    if (mo->flags & MF_JUSTHIT) {
        mo->flags &= ~MF_JUSTHIT;
        return 1;
    }
    if (mo->reactiontime) return 0;
    dist = (s16)(dist_to(mo, mo->target) >> 0) - 64;
    if (!in->melee) dist -= 128;
    if (mo->cls == C_LOSTSOUL) dist >>= 1;
    if (dist > 200) dist = 200;
    if (prandom() < dist) return 0;
    return 1;
}

/* ---- attacks ---- */

static void hitscan(mobj_t *mo, s16 shots)
{
    mobj_t *t = mo->target;
    s16 dist;
    if (!t) return;
    face_target(mo);
    dist = (s16)dist_to(mo, t);
    while (shots--) {
        s16 err = (s16)((s16)(prandom() - prandom()) << 4);
        s16 miss = (s16)(MUL16(dist, fsin((angle_t)err)) >> 14);
        if (ABS(miss) <= t->radius + 4) damage_mobj(t, mo, mo, (s16)(((prandom() % 5) + 1) * 3));
    }
    noise_alert(t, mo);
}

static void do_attack(mobj_t *mo)
{
    const info_t *in = &info[mo->cls];
    mobj_t *t = mo->target;
    if (!t) return;
    face_target(mo);
    if (check_melee(mo)) {
        s16 dice = in->melee, d = (s16)((prandom() & 7) + 1);
        if (mo->cls == C_DEMON || mo->cls == C_SPECTRE) d = (s16)((prandom() % 10) + 1);
        damage_mobj(t, mo, mo, d * dice);
        return;
    }
    if (mo->cls == C_LOSTSOUL) {
        mo->flags |= MF_SKULLFLY;
        mo->momx = (s32)20 * fcos(mo->angle) << 2;
        mo->momy = (s32)20 * fsin(mo->angle) << 2;
        {
            u16 d = dist_to(mo, t) / 20;
            if (d < 1) d = 1;
            mo->momz = (t->z + ((s32)t->height << 15) - mo->z) / (s32)d;
        }
        return;
    }
    if (in->missile) {
        spawn_missile(mo, t, in->missile, mo->angle);
        if (mo->cls == C_BARON || mo->cls == C_CACODEMON) noise_alert(t, mo);
        return;
    }
    if (in->shots) hitscan(mo, in->shots);
}

/* ---- the think ---- */

void monster_think(mobj_t *mo)
{
    if (mo->tics > 0 && --mo->tics > 0) {
        /* lost souls fly on their own momentum */
        return;
    }
    switch (mo->state) {
    case S_IDLE: {
        sector_t *sec = &lvl.sectors[mo->ss->sector];
        mo->tics = 10;
        mo->frame = FR_STAND;
        if (!info[mo->cls].speed) return;              /* barrels */
        if (sec->soundtarget && (sec->soundtarget->flags & MF_SHOOTABLE)) {
            mo->target = sec->soundtarget;
            if ((mo->flags & MF_AMBUSH) && !check_sight(mo, mo->target)) return;
        } else if (!look_for_player(mo, 0))
            return;
        mo->state = S_CHASE;
        mo->tics = 1;
        break;
    }
    case S_CHASE:
        mo->tics = mo->cls == C_DEMON || mo->cls == C_SPECTRE ? 2 : 4;
        mo->frame = mo->frame == FR_WALK ? FR_STAND : FR_WALK;
        if (mo->flags & MF_SKULLFLY) return;
        if (mo->reactiontime) mo->reactiontime--;
        if (mo->threshold) {
            if (!mo->target || mo->target->health <= 0) mo->threshold = 0;
            else mo->threshold--;
        }
        if (mo->movedir < 8) {
            /* turn towards the walking direction */
            angle_t want = (angle_t)(mo->movedir << 13);
            s16 delta = (s16)(want - mo->angle);
            if (delta > 0) mo->angle += ANG45 / 2 > delta ? (angle_t)delta : ANG45 / 2;
            else if (delta < 0) mo->angle -= ANG45 / 2 > -delta ? (angle_t)(-delta) : ANG45 / 2;
        }
        if (!mo->target || !(mo->target->flags & MF_SHOOTABLE) || mo->target->health <= 0) {
            mo->target = NULL;
            if (look_for_player(mo, 1)) return;
            mo->state = S_IDLE;
            mo->tics = 10;
            return;
        }
        if (mo->attackdone) {
            mo->attackdone = 0;
            new_chase_dir(mo);
            return;
        }
        if (check_melee(mo) || (!mo->movecount && check_missile(mo))) {
            mo->state = S_ATTACK;
            mo->frame = FR_ATTACK;
            mo->tics = info[mo->cls].attack_tics / 2;
            mo->movecount = 0;
            return;
        }
        if (--mo->movecount < 0 || !do_move(mo)) new_chase_dir(mo);
        break;
    case S_ATTACK:
        if (!mo->movecount) {
            do_attack(mo);
            mo->movecount = 1;
            mo->tics = info[mo->cls].attack_tics / 2;
            /* chaingunners would refire; others go back to chasing */
            return;
        }
        mo->state = S_CHASE;
        mo->attackdone = 1;
        mo->movecount = 0;
        mo->tics = 1;
        break;
    case S_PAIN:
        mo->state = S_CHASE;
        mo->frame = FR_WALK;
        mo->tics = 1;
        break;
    case S_DYING:
        mo->state = S_DEAD;
        mo->frame = FR_DEAD;
        mo->tics = -1;
        break;
    case S_EXPLODE:
        if (mo->cls == C_BARREL) {
            mo->state = S_DEAD;
            mo->flags |= MF_NODRAW;
            mo->tics = -1;
            remove_mobj(mo);
        } else
            remove_mobj(mo);
        break;
    default:
        mo->tics = -1;
        break;
    }
}

void boss_death(mobj_t *mo)
{
    s16 i;
    if (mo->cls != C_BARON || lvl.name[0] != 'E' || lvl.name[3] != '8') return;
    for (i = 0; i < mobj_top; i++)
        if (mobjs[i].inuse && mobjs[i].cls == C_BARON && mobjs[i].health > 0) return;
    ev_lower_floor_tag(666);
}
