/* Functions shared between the game modules. */
#ifndef SPEC_H
#define SPEC_H
#include "game.h"

#define ONFLOORZ ((s32)0x80000000L)
#define ONCEILINGZ ((s32)0x7fffffffL)

/* mobj.c */
extern s32 tmfloorz, tmceilingz, tmdropoffz;
extern line_t *spechit[16];
extern s16 numspechit;
extern line_t *blockline;
extern mobj_t *blockthing, *linetarget;
int check_position(mobj_t *mo, s32 x, s32 y);
int try_move(mobj_t *mo, s32 x, s32 y);
int thing_height_clip(mobj_t *mo);
void mobj_physics(mobj_t *mo);
int check_sight(mobj_t *a, mobj_t *b);
void damage_mobj(mobj_t *target, mobj_t *inflictor, mobj_t *source, s16 damage);
void kill_mobj(mobj_t *source, mobj_t *target);
void radius_attack(mobj_t *spot, mobj_t *source, s16 damage);
mobj_t *aim_line(mobj_t *t1, angle_t a, s16 range, s16 slop);
mobj_t *line_attack(mobj_t *t1, angle_t a, s16 range, s16 damage);
mobj_t *spawn_missile(mobj_t *source, mobj_t *dest, int cls, angle_t an);
void spawn_puff(s32 x, s32 y, s32 z);

/* ai.c */
void monster_think(mobj_t *mo);
void noise_alert(mobj_t *target, mobj_t *emitter);
void boss_death(mobj_t *mo);

/* player.c */
void player_think(u16 buttons, u8 weaponkey);
void touch_special(mobj_t *special);
void player_reset(int keep);
void set_message(const char *s);

/* spec.c */
void cross_special_line(line_t *line, int side, mobj_t *thing);
int use_special_line(mobj_t *thing, line_t *line, int side);
void shoot_special_line(mobj_t *thing, line_t *line);
void use_lines(void);
void movers_think(void);
void movers_clear(void);
void player_in_special_sector(void);
extern u8 exit_requested;

#endif
