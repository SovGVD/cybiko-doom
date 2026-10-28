/* PC speaker sounds on the Cybiko's piezo: one sound at a time, as Doom
 * played them on a PC speaker. The tones come from the WAD's DP* lumps,
 * which tools/wad2cy.py puts in doom_snd.dat (no file: no sound). */
#ifndef SOUND_H
#define SOUND_H
#include "doomtype.h"

/* in the order of tools/wad2cy.py SOUNDS */
enum {
    SFX_NONE,
    SFX_PISTOL, SFX_SHOTGN, SFX_PUNCH, SFX_SAWFUL, SFX_RLAUNC, SFX_PLASMA, SFX_BFG,
    SFX_BAREXP, SFX_FIRSHT, SFX_FIRXPL,
    SFX_DOROPN, SFX_DORCLS, SFX_BDOPN, SFX_BDCLS, SFX_PSTART, SFX_PSTOP,
    SFX_SWTCHN, SFX_SWTCHX,
    SFX_ITEMUP, SFX_WPNUP, SFX_GETPOW, SFX_TELEPT, SFX_NOWAY, SFX_OOF,
    SFX_PLPAIN, SFX_PLDETH, SFX_SLOP,
    SFX_POSIT1, SFX_BGSIT1, SFX_SGTSIT, SFX_CACSIT, SFX_BRSSIT,
    SFX_POPAIN, SFX_DMPAIN,
    SFX_PODTH1, SFX_BGDTH1, SFX_SGTDTH, SFX_CACDTH, SFX_BRSDTH, SFX_SKLDTH,
    SFX_SGTATK, SFX_CLAW, SFX_SKLATK,
    NUMSFX
};

/* what a monster says, by class (game.h): when it sees the player, in
 * pain, dying, and its melee attack */
enum { SAY_SIGHT, SAY_PAIN, SAY_DEATH, SAY_MELEE };

struct mobj_s;
extern u8 sound_on;             /* the title menu's SOUND */
void sound_load(void);          /* doom_snd.dat (pack_open calls it) */
int sound_loaded(void);
void sound_start(int sfx);      /* the player's own sounds */
void sound_at(const struct mobj_s *mo, int sfx);    /* heard if within reach */
void sound_say(const struct mobj_s *mo, int what);  /* a monster's (SAY_*) */
void sound_update(void);        /* call often: steps the tones (140 a second) */
void sound_stop(void);

#endif
