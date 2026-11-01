/* Game state shared by the game logic, the renderer and the HUD. */
#ifndef GAME_H
#define GAME_H
#include "doomtype.h"
#include "level.h"

#define TICRATE 35

/* thing classes, in the order of tools/wad2cy.py CLASS_ORDER */
enum {
    C_PLAYER, C_ZOMBIE, C_SERGEANT, C_IMP, C_DEMON, C_SPECTRE, C_CACODEMON,
    C_BARON, C_LOSTSOUL, C_BARREL,
    C_SHOTGUN, C_CHAINGUN, C_LAUNCHER, C_PLASMA, C_CHAINSAW, C_BFG,
    C_CLIP, C_AMMOBOX, C_SHELLS, C_SHELLBOX, C_ROCKET, C_ROCKETBOX, C_CELL,
    C_CELLPACK, C_BACKPACK,
    C_STIMPACK, C_MEDIKIT, C_HEALTHBONUS, C_ARMORBONUS, C_GREENARMOR,
    C_BLUEARMOR, C_SOULSPHERE, C_INVULN, C_BERSERK, C_INVIS, C_RADSUIT, C_MAP,
    C_VISOR,
    C_BLUECARD, C_YELLOWCARD, C_REDCARD, C_BLUESKULL, C_YELLOWSKULL, C_REDSKULL,
    C_LAMP, C_TECHCOLUMN, C_GREENPILLAR, C_REDPILLAR, C_SHORTGREEN, C_SHORTRED,
    C_CANDELABRA, C_EVILEYE, C_FLOATSKULL, C_BURNTTREE, C_BLUETORCH,
    C_GREENTORCH, C_REDTORCH, C_STUB, C_BIGTREE, C_SHORTBLUETORCH,
    C_SHORTGREENTORCH, C_SHORTREDTORCH, C_BURNINGBARREL,
    C_CANDLE, C_BLOODYMESS, C_BLOODYMESS2, C_DEADPLAYER, C_DEADZOMBIE,
    C_DEADSERGEANT, C_DEADIMP, C_DEADDEMON, C_POOLOFBLOOD,
    C_IMPALED, C_TWITCHING, C_HEADONSTICK, C_HEADSONSTICK, C_HEADCANDLES,
    C_HANGINGBODY, C_HANGINGARMS, C_HANGINGLEG1, C_HANGINGLEGS, C_HANGINGLEG2,
    C_FIREBALL, C_PLASMABALL, C_BARONBALL, C_ROCKETSHOT, C_PUFF, C_BLOOD,
    C_W_FIST, C_W_PISTOL, C_W_SHOTGUN, C_W_CHAINGUN, C_W_LAUNCHER, C_W_PLASMA,
    C_W_CHAINSAW, C_W_BFG,
    C_TELEPORTMAN,          /* not drawn; teleport destinations */
    NUMCLASSES
};

/* sprite frames of a class (wad2cy.py: stand, walk, attack, pain, dead) */
enum { FR_STAND, FR_WALK, FR_ATTACK, FR_PAIN, FR_DEAD };

/* mobj flags */
#define MF_SOLID 0x0001
#define MF_SHOOTABLE 0x0002
#define MF_PICKUP 0x0004        /* an item the player can take */
#define MF_MISSILE 0x0008
#define MF_COUNTKILL 0x0010
#define MF_COUNTITEM 0x0020
#define MF_FLOAT 0x0040
#define MF_SPAWNCEILING 0x0080
#define MF_SHADOW 0x0100        /* spectre: drawn dotted */
#define MF_CORPSE 0x0200
#define MF_AMBUSH 0x0400
#define MF_NOGRAVITY 0x0800
#define MF_JUSTHIT 0x1000
#define MF_NODRAW 0x2000
#define MF_DROPPED 0x4000
#define MF_SKULLFLY 0x8000

/* mobj states */
enum { S_IDLE, S_CHASE, S_ATTACK, S_PAIN, S_DYING, S_DEAD, S_FLY, S_EXPLODE, S_STILL };

typedef struct mobj_s {
    s32 x, y, z;                /* 16.16 */
    s32 momx, momy, momz;
    s32 floorz, ceilingz;
    angle_t angle;
    s16 cls;
    s16 health;
    u16 flags;
    u8 state, frame;
    s16 tics;
    s16 movecount;
    u8 movedir, reactiontime;
    s8 threshold;
    u8 attackdone;
    struct mobj_s *target;
    struct mobj_s *tracer;
    struct mobj_s *snext;       /* things in the same subsector */
    struct mobj_s *bnext;       /* things in the same blockmap cell */
    subsector_t *ss;
    s16 bcell;                  /* blockmap cell, -1 if none */
    s16 radius, height;         /* map units */
    u16 validcount;
    u8 inuse;
    u8 spawnthing;              /* index of the map thing, for respawns */
} mobj_t;

typedef struct {
    s16 radius, height;
    s16 health;
    s16 speed;                  /* map units per tic (monsters), 16.16 >> 16 */
    u8 painchance;              /* of 256 */
    u16 mass;
    u16 flags;
    u8 reaction;                /* tics before the first attack */
    u8 melee;                   /* melee damage dice: n d 8 */
    u8 missile;                 /* missile class, 0 = hitscan */
    u8 shots;                   /* hitscan pellets */
    u8 attack_tics;
    u8 drop;                    /* class dropped on death, 0 none */
} info_t;

extern const info_t info[NUMCLASSES];

#define MAXMOBJS 512

typedef struct {
    mobj_t *mo;
    s16 health, armor;
    u8 armortype;               /* 0, 1 green, 2 blue */
    u8 weapon, pending;         /* 0 fist .. 7 */
    u8 owned[8];
    s16 ammo[4], maxammo[4];    /* clip, shell, rocket, cell */
    u8 cards;                   /* bits: blue, yellow, red cards; then skulls */
    u8 backpack;
    s16 refire;
    s16 attackdown;
    s16 weapontics;             /* until the weapon can fire again */
    u8 weaponframe;             /* 0 ready, 1 firing */
    s16 damagecount, bonuscount;
    s16 powers[6];              /* invuln, strength, invis, ironfeet, allmap, light */
    s32 viewz, viewheight, deltaviewheight;
    s32 bob;
    u8 dead;
    s16 kills, items, secrets;
    mobj_t *attacker;           /* who hurt the player last (the face looks there) */
    char message[24];
    s16 messagetics;
    u8 usedown;
    u8 cheat;
} player_t;

extern player_t player;
extern mobj_t mobjs[MAXMOBJS];
extern s16 mobj_top;             /* no mobj in use at or above this index */
extern s16 totalkills, totalitems, totalsecrets;
extern u32 gametic;
extern s16 levelidx;
extern u8 skill;                /* 0-4 as in Doom */

/* input for one tic */
#define BT_FORWARD 1
#define BT_BACK 2
#define BT_LEFT 4
#define BT_RIGHT 8
#define BT_STRAFEL 16
#define BT_STRAFER 32
#define BT_FIRE 64
#define BT_USE 128
#define BT_RUN 256
#define BT_STRAFE 512
#define BT_NEXTWEAPON 1024
#define BT_MAP 2048
#define BT_PREVWEAPON 4096

int game_start_level(int idx);     /* 0 if the map does not load */
void game_tic(u16 buttons, u8 weaponkey);
int game_exit_requested(void);  /* 1 normal exit, 2 secret exit */

/* game.c helpers used elsewhere */
mobj_t *spawn_mobj(s32 x, s32 y, s32 z, int cls);
void remove_mobj(mobj_t *mo);
void set_position(mobj_t *mo);
void unset_position(mobj_t *mo);

#endif
