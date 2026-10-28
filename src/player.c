/* The player: movement, view height, weapons and picking things up
 * (Doom's p_user.c, p_pspr.c and p_inter.c, simplified). */
#include "game.h"
#include "spec.h"
#include "sound.h"

player_t player;

enum { AM_CLIP, AM_SHELL, AM_MISL, AM_CELL, AM_NOAMMO = 255 };
enum { WP_FIST, WP_PISTOL, WP_SHOTGUN, WP_CHAINGUN, WP_MISSILE, WP_PLASMA, WP_BFG, WP_CHAINSAW };

static const struct {
    u8 ammo, per, tics;
} weapons[8] = {
    {AM_NOAMMO, 0, 14}, {AM_CLIP, 1, 14}, {AM_SHELL, 1, 37}, {AM_CLIP, 1, 4},
    {AM_MISL, 1, 20}, {AM_CELL, 1, 3}, {AM_CELL, 40, 60}, {AM_NOAMMO, 0, 4},
};
static const s16 maxammo[4] = { 200, 50, 50, 300 };
static const s16 clipammo[4] = { 10, 4, 1, 20 };

void set_message(const char *s)
{
    int i;
    for (i = 0; s[i] && i < (int)sizeof player.message - 1; i++) player.message[i] = s[i];
    player.message[i] = 0;
    player.messagetics = 3 * TICRATE;
}

/* keep = 0: a new game; 1: the next level (keep weapons) */
void player_reset(int keep)
{
    int i;
    if (!keep || player.dead) {
        player.health = 100;
        player.armor = 0;
        player.armortype = 0;
        for (i = 0; i < 8; i++) player.owned[i] = 0;
        player.owned[WP_FIST] = player.owned[WP_PISTOL] = 1;
        for (i = 0; i < 4; i++) {
            player.ammo[i] = 0;
            player.maxammo[i] = maxammo[i];
        }
        player.ammo[AM_CLIP] = 50;
        player.weapon = player.pending = WP_PISTOL;
        player.backpack = 0;
    }
    player.cards = 0;
    player.dead = 0;
    player.kills = player.items = player.secrets = 0;
    player.damagecount = player.bonuscount = 0;
    player.attacker = NULL;
    for (i = 0; i < 6; i++) player.powers[i] = 0;
    player.viewheight = 41 * FRACUNIT;
    player.deltaviewheight = 0;
    player.weapontics = 0;
    player.weaponframe = 0;
    player.messagetics = 0;
    player.refire = 0;
}

/* ---- weapons ---- */

static int has_ammo(int w)
{
    if (weapons[w].ammo == AM_NOAMMO) return 1;
    return player.ammo[weapons[w].ammo] >= weapons[w].per;
}

static void pick_weapon(void)
{
    static const u8 order[] = { WP_PLASMA, WP_CHAINGUN, WP_SHOTGUN, WP_PISTOL, WP_CHAINSAW,
                                WP_MISSILE, WP_BFG, WP_FIST };
    int i;
    for (i = 0; i < (int)sizeof order; i++)
        if (player.owned[order[i]] && has_ammo(order[i])) {
            player.pending = order[i];
            return;
        }
}

static void fire_weapon(void)
{
    mobj_t *mo = player.mo;
    int w = player.weapon;
    angle_t an = mo->angle;
    s16 i;
    if (!has_ammo(w)) {
        pick_weapon();
        return;
    }
    if (weapons[w].ammo != AM_NOAMMO) player.ammo[weapons[w].ammo] -= weapons[w].per;
    player.weapontics = weapons[w].tics;
    player.weaponframe = 1;
    noise_alert(mo, mo);
    {
        static const u8 shot[8] = { 0, SFX_PISTOL, SFX_SHOTGN, SFX_PISTOL, SFX_RLAUNC,
                                    SFX_PLASMA, SFX_BFG, SFX_SAWFUL };
        if (shot[w]) sound_start(shot[w]);
    }
    switch (w) {
    case WP_FIST:
    case WP_CHAINSAW: {
        s16 dmg = (s16)(((prandom() % 10) + 1) << 1);
        if (w == WP_FIST && player.powers[1]) dmg *= 10;
        if (aim_line(mo, an, 64 + 20, 8)) {
            if (w == WP_FIST) sound_start(SFX_PUNCH);
            damage_mobj(linetarget, mo, mo, dmg);
            mo->angle = point_to_angle(linetarget->x - mo->x, linetarget->y - mo->y);
        }
        break;
    }
    case WP_PISTOL:
    case WP_CHAINGUN:
        if (!aim_line(mo, an, 2048, 8)) an += (angle_t)((s16)(prandom() - prandom()) << 2);
        line_attack(mo, an, 2048, (s16)(5 * ((prandom() % 3) + 1)));
        break;
    case WP_SHOTGUN:
        for (i = 0; i < 7; i++)
            line_attack(mo, an + (angle_t)((s16)(prandom() - prandom()) << 3), 2048,
                        (s16)(5 * ((prandom() % 3) + 1)));
        break;
    case WP_MISSILE:
    case WP_PLASMA:
    case WP_BFG: {
        mobj_t *t = aim_line(mo, an, 1024, 16);
        mobj_t *m = spawn_missile(mo, t, w == WP_MISSILE ? C_ROCKETSHOT : C_PLASMABALL, an);
        if (m && w == WP_BFG) m->momx <<= 1, m->momy <<= 1;
        break;
    }
    }
}

static void weapon_think(u16 buttons, u8 weaponkey)
{
    if (weaponkey && weaponkey <= 8 && player.owned[weaponkey - 1]) player.pending = weaponkey - 1;
    if (buttons & (BT_NEXTWEAPON | BT_PREVWEAPON)) {
        int i, d = (buttons & BT_NEXTWEAPON) ? 1 : 7;
        for (i = 1; i <= 8; i++) {
            int w = (player.weapon + i * d) & 7;
            if (player.owned[w] && has_ammo(w)) {
                player.pending = (u8)w;
                break;
            }
        }
    }
    if (player.weapontics > 0) {
        player.weapontics--;
        if (player.weapontics < weapons[player.weapon].tics / 2 + 1) player.weaponframe = 0;
        return;
    }
    player.weaponframe = 0;
    if (player.pending != player.weapon) {
        player.weapon = player.pending;
        player.weapontics = 6;
        return;
    }
    if (buttons & BT_FIRE) fire_weapon();
}

/* ---- pickups ---- */

static int give_ammo(int am, s16 n)
{
    if (player.ammo[am] >= player.maxammo[am]) return 0;
    if (skill == 0 || skill == 4) n <<= 1;
    player.ammo[am] += n;
    if (player.ammo[am] > player.maxammo[am]) player.ammo[am] = player.maxammo[am];
    return 1;
}

static int give_weapon(int w, int dropped)
{
    int got = !player.owned[w];
    int ammo = weapons[w].ammo != AM_NOAMMO
        && give_ammo(weapons[w].ammo, (s16)(clipammo[weapons[w].ammo] * (dropped ? 1 : 2)));
    if (got) {
        player.owned[w] = 1;
        player.pending = (u8)w;
    }
    return got || ammo;
}

void touch_special(mobj_t *sp)
{
    mobj_t *pm = player.mo;
    s32 dz = sp->z - pm->z;
    const char *msg = NULL;
    int dropped = (sp->flags & MF_DROPPED) != 0;
    if (dz > ((s32)pm->height << FRACBITS) || dz < -8 * FRACUNIT) return;
    if (player.dead) return;
    switch (sp->cls) {
    case C_GREENARMOR:
        if (player.armor >= 100) return;
        player.armor = 100; player.armortype = 1; msg = "GREEN ARMOR"; break;
    case C_BLUEARMOR:
        if (player.armor >= 200) return;
        player.armor = 200; player.armortype = 2; msg = "MEGA ARMOR"; break;
    case C_HEALTHBONUS:
        if (++player.health > 200) player.health = 200;
        msg = "HEALTH BONUS"; break;
    case C_ARMORBONUS:
        if (++player.armor > 200) player.armor = 200;
        if (!player.armortype) player.armortype = 1;
        msg = "ARMOR BONUS"; break;
    case C_SOULSPHERE:
        player.health += 100;
        if (player.health > 200) player.health = 200;
        msg = "SUPERCHARGE!"; break;
    case C_BLUECARD: case C_YELLOWCARD: case C_REDCARD:
        player.cards |= (u8)(1 << (sp->cls - C_BLUECARD));
        msg = sp->cls == C_BLUECARD ? "BLUE KEYCARD" : sp->cls == C_YELLOWCARD ? "YELLOW KEYCARD" : "RED KEYCARD";
        break;
    case C_BLUESKULL: case C_YELLOWSKULL: case C_REDSKULL:
        player.cards |= (u8)(8 << (sp->cls - C_BLUESKULL));
        msg = "SKULL KEY"; break;
    case C_STIMPACK:
        if (player.health >= 100) return;
        player.health += 10; if (player.health > 100) player.health = 100;
        msg = "STIMPACK"; break;
    case C_MEDIKIT:
        if (player.health >= 100) return;
        player.health += 25; if (player.health > 100) player.health = 100;
        msg = "MEDIKIT"; break;
    case C_INVULN: player.powers[0] = 30 * TICRATE; msg = "INVULNERABILITY!"; break;
    case C_BERSERK:
        player.powers[1] = 1;
        if (player.health < 100) player.health = 100;
        player.pending = WP_FIST;
        msg = "BERSERK!"; break;
    case C_INVIS: player.powers[2] = 60 * TICRATE; msg = "INVISIBILITY"; break;
    case C_RADSUIT: player.powers[3] = 60 * TICRATE; msg = "RADIATION SUIT"; break;
    case C_MAP: player.powers[4] = 1; msg = "COMPUTER MAP"; break;
    case C_VISOR: player.powers[5] = 120 * TICRATE; msg = "LIGHT VISOR"; break;
    case C_CLIP:
        if (!give_ammo(AM_CLIP, dropped ? 5 : 10)) return;
        msg = "CLIP"; break;
    case C_AMMOBOX:
        if (!give_ammo(AM_CLIP, 50)) return;
        msg = "BOX OF BULLETS"; break;
    case C_SHELLS:
        if (!give_ammo(AM_SHELL, 4)) return;
        msg = "4 SHELLS"; break;
    case C_SHELLBOX:
        if (!give_ammo(AM_SHELL, 20)) return;
        msg = "BOX OF SHELLS"; break;
    case C_ROCKET:
        if (!give_ammo(AM_MISL, 1)) return;
        msg = "A ROCKET"; break;
    case C_ROCKETBOX:
        if (!give_ammo(AM_MISL, 5)) return;
        msg = "BOX OF ROCKETS"; break;
    case C_CELL:
        if (!give_ammo(AM_CELL, 20)) return;
        msg = "ENERGY CELL"; break;
    case C_CELLPACK:
        if (!give_ammo(AM_CELL, 100)) return;
        msg = "ENERGY PACK"; break;
    case C_BACKPACK: {
        int i;
        if (!player.backpack) {
            for (i = 0; i < 4; i++) player.maxammo[i] *= 2;
            player.backpack = 1;
        }
        for (i = 0; i < 4; i++) give_ammo(i, clipammo[i]);
        msg = "BACKPACK"; break;
    }
    case C_SHOTGUN: if (!give_weapon(WP_SHOTGUN, dropped)) return; msg = "SHOTGUN!"; break;
    case C_CHAINGUN: if (!give_weapon(WP_CHAINGUN, dropped)) return; msg = "CHAINGUN!"; break;
    case C_LAUNCHER: if (!give_weapon(WP_MISSILE, dropped)) return; msg = "ROCKET LAUNCHER!"; break;
    case C_PLASMA: if (!give_weapon(WP_PLASMA, dropped)) return; msg = "PLASMA GUN!"; break;
    case C_CHAINSAW: if (!give_weapon(WP_CHAINSAW, dropped)) return; msg = "CHAINSAW!"; break;
    case C_BFG: if (!give_weapon(WP_BFG, dropped)) return; msg = "BFG9000!"; break;
    default:
        return;
    }
    if (sp->flags & MF_COUNTITEM) player.items++;
    player.bonuscount += 6;
    sound_start(sp->cls >= C_SHOTGUN && sp->cls <= C_BFG ? SFX_WPNUP
                : (sp->cls >= C_SOULSPHERE && sp->cls <= C_VISOR) ? SFX_GETPOW : SFX_ITEMUP);
    if (msg) set_message(msg);
    remove_mobj(sp);
}

/* ---- movement ---- */

static void thrust(angle_t a, s32 move)
{
    /* move is in units/tic * 2048 like Doom's forwardmove */
    player.mo->momx += (move >> 8) * fcos(a) >> 6;
    player.mo->momy += (move >> 8) * fsin(a) >> 6;
}

static void calc_height(void)
{
    mobj_t *mo = player.mo;
    s32 bob;
    player.bob = ((mo->momx >> 8) * (mo->momx >> 8) + (mo->momy >> 8) * (mo->momy >> 8)) >> 2;
    if (player.bob > 16 * FRACUNIT) player.bob = 16 * FRACUNIT;
    bob = ((player.bob >> 13) * fsin((angle_t)(gametic << 11))) >> 2;
    if (player.dead) {
        if (player.viewheight > 6 * FRACUNIT) player.viewheight -= FRACUNIT;
    } else {
        player.viewheight += player.deltaviewheight;
        if (player.viewheight > 41 * FRACUNIT) {
            player.viewheight = 41 * FRACUNIT;
            player.deltaviewheight = 0;
        }
        if (player.viewheight < 20 * FRACUNIT) {
            player.viewheight = 20 * FRACUNIT;
            if (player.deltaviewheight <= 0) player.deltaviewheight = 1;
        }
        if (player.deltaviewheight) {
            player.deltaviewheight += FRACUNIT / 4;
            if (!player.deltaviewheight) player.deltaviewheight = 1;
        }
    }
    player.viewz = mo->z + player.viewheight + (player.dead ? 0 : bob);
    if (player.viewz > mo->ceilingz - 4 * FRACUNIT) player.viewz = mo->ceilingz - 4 * FRACUNIT;
}

void player_think(u16 buttons, u8 weaponkey)
{
    mobj_t *mo = player.mo;
    if (player.messagetics) player.messagetics--;
    if (player.damagecount) player.damagecount--;
    if (player.bonuscount) player.bonuscount--;
    if (player.powers[0]) player.powers[0]--;
    if (player.powers[2] && !--player.powers[2]) mo->flags &= ~MF_SHADOW;
    if (player.powers[3]) player.powers[3]--;
    if (player.powers[5]) player.powers[5]--;
    if (player.dead) {
        calc_height();
        return;
    }
    if (player.powers[2]) mo->flags |= MF_SHADOW;
    {
        s32 fwd = 0, side = 0;
        s32 speed = (buttons & BT_RUN) ? 0x32L * 2048 : 0x19L * 2048;
        s32 sidespeed = (buttons & BT_RUN) ? 0x28L * 2048 : 0x18L * 2048;
        angle_t turn = (buttons & BT_RUN) ? 1280 : 640;
        if (buttons & BT_FORWARD) fwd += speed;
        if (buttons & BT_BACK) fwd -= speed;
        if (buttons & BT_STRAFE) {
            if (buttons & BT_LEFT) side -= sidespeed;
            if (buttons & BT_RIGHT) side += sidespeed;
        } else {
            if (buttons & BT_LEFT) mo->angle += turn;
            if (buttons & BT_RIGHT) mo->angle -= turn;
        }
        if (buttons & BT_STRAFEL) side -= sidespeed;
        if (buttons & BT_STRAFER) side += sidespeed;
        if (mo->z <= mo->floorz) {
            if (fwd) thrust(mo->angle, fwd);
            if (side) thrust(mo->angle - ANG90, side);
        }
        if (fwd || side) mo->frame = (u8)((gametic >> 2) & 1);
    }
    if (buttons & BT_USE) {
        if (!player.usedown) {
            use_lines();
            player.usedown = 1;
        }
    } else
        player.usedown = 0;
    weapon_think(buttons, weaponkey);
    player_in_special_sector();
}

void player_view(void)
{
    calc_height();
}
