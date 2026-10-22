/* Starting a level and running one game tic. */
#include "game.h"
#include "spec.h"

s16 totalkills, totalitems, totalsecrets;
u32 gametic;
s16 levelidx;
u8 skill = 2;

void spawn_specials(void);
void player_view(void);

static void spawn_map_thing(const mapthing_t *mt, s16 index)
{
    mobj_t *mo;
    s16 bit;
    const info_t *in;
    if (mt->cls == C_PLAYER) {
        mo = spawn_mobj((s32)mt->x << FRACBITS, (s32)mt->y << FRACBITS, ONFLOORZ, C_PLAYER);
        if (!mo) return;
        mo->angle = (angle_t)(((s32)mt->angle * 65536L) / 360);
        player.mo = mo;
        return;
    }
    bit = skill <= 1 ? 1 : skill == 2 ? 2 : 4;
    if (!(mt->flags & bit)) return;
    if (mt->cls < 0 || mt->cls >= NUMCLASSES) return;
    in = &info[mt->cls];
    mo = spawn_mobj((s32)mt->x << FRACBITS, (s32)mt->y << FRACBITS,
                    (in->flags & MF_SPAWNCEILING) ? ONCEILINGZ : ONFLOORZ, mt->cls);
    if (!mo) return;
    mo->spawnthing = (u8)index;
    mo->angle = (angle_t)(((s32)(mt->angle / 45) * 45 * 65536L) / 360);
    if (mt->flags & 8) mo->flags |= MF_AMBUSH;
    if (mo->flags & MF_COUNTKILL) totalkills++;
    if (mo->flags & MF_COUNTITEM) totalitems++;
    if (in->speed && !(in->flags & MF_MISSILE)) {
        mo->state = S_IDLE;
        mo->tics = 1 + (index & 7);
    } else
        mo->state = S_STILL;
    if (mo->cls == C_BARREL) mo->state = S_IDLE;
    if (mo->cls == C_BURNINGBARREL || mo->cls == C_LAMP) mo->frame = 0;
}

void game_start_level(int idx)
{
    s16 i;
    levelidx = (s16)idx;
    mset(mobjs, 0, sizeof mobjs);
    mobj_top = 0;
    movers_clear();
    if (!level_load(idx)) return;
    totalkills = totalitems = totalsecrets = 0;
    exit_requested = 0;
    player.mo = NULL;
    for (i = 0; i < lvl.nthings; i++) spawn_map_thing(&lvl.things[i], i);
    if (!player.mo) {
        /* no start on this map: put the player at the first vertex */
        player.mo = spawn_mobj((s32)lvl.vertexes[0].x << FRACBITS, (s32)lvl.vertexes[0].y << FRACBITS,
                               ONFLOORZ, C_PLAYER);
    }
    player.mo->health = player.health;
    spawn_specials();
    player.viewheight = 41 * FRACUNIT;
    player.viewz = player.mo->z + player.viewheight;
    gametic = 0;
}

int game_exit_requested(void)
{
    return exit_requested;
}

void game_tic(u16 buttons, u8 weaponkey)
{
    s16 i;
    mobj_t *pm = player.mo;
    if (pm->reactiontime) {
        pm->reactiontime--;
        buttons &= BT_MAP;
    }
    player_think(buttons, weaponkey);
    if (pm->health != player.health) pm->health = player.health;
    for (i = 0; i < mobj_top; i++) {
        mobj_t *mo = &mobjs[i];
        if (!mo->inuse) continue;
        if (mo->momx || mo->momy || mo->momz || mo->z != mo->floorz || (mo->flags & MF_SKULLFLY))
            mobj_physics(mo);
        if (!mo->inuse || mo == pm) continue;
        if (mo->state != S_STILL && mo->state != S_DEAD && mo->state != S_FLY) monster_think(mo);
        else if (mo->state == S_FLY && mo->tics > 0 && !--mo->tics) remove_mobj(mo);
    }
    movers_think();
    player_view();
    gametic++;
}
