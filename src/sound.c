/* PC speaker sounds (sound.h). A sound is a list of tones, 140 a second,
 * each 0 (silence) or 1-127, an index into Doom's PC speaker table below;
 * the hardware plays one tone at a time (hw_tone) and sound_update moves
 * to the tone the clock says. */
#include "sound.h"
#include "game.h"
#include "hw.h"

u8 sound_on = 1;

/* doom_snd.dat: "CYSN", u16 version (1), u16 sound count, u16 tone bytes,
 * then per sound u16 offset and u16 length in the tones, then the tones */
static const u8 *tones;
static u16 sfx_off[NUMSFX], sfx_len[NUMSFX];

/* Doom's PIT divisors for the tone indexes (1193181 Hz / divisor), as
 * Chocolate Doom has them */
static const u16 divisors[128] = {
    0,
    6818, 6628, 6449, 6279, 6087, 5906, 5736, 5575,
    5423, 5279, 5120, 4971, 4830, 4697, 4554, 4435,
    4307, 4186, 4058, 3950, 3836, 3728, 3615, 3519,
    3418, 3323, 3224, 3131, 3043, 2960, 2875, 2794,
    2711, 2633, 2560, 2485, 2415, 2348, 2281, 2213,
    2153, 2089, 2032, 1975, 1918, 1864, 1810, 1757,
    1709, 1659, 1612, 1565, 1521, 1478, 1435, 1395,
    1355, 1316, 1280, 1242, 1207, 1173, 1140, 1107,
    1075, 1045, 1015, 986, 959, 931, 905, 879,
    854, 829, 806, 783, 760, 739, 718, 697,
    677, 658, 640, 621, 604, 586, 570, 553,
    538, 522, 507, 493, 479, 465, 452, 439,
    427, 415, 403, 391, 380, 369, 359, 348,
    339, 329, 319, 310, 302, 293, 285, 276,
    269, 261, 253, 246, 239, 232, 226, 219,
    213, 207, 201, 195, 190, 184, 179,
};

/* Doom's priorities: a sound cuts in on one with the same or a bigger number */
static const u8 priority[NUMSFX] = {
    255,
    64, 64, 64, 64, 64, 64, 64,         /* pistol .. bfg */
    32, 70, 70,                         /* barexp firsht firxpl */
    100, 100, 100, 100, 100, 100,       /* doors, plats */
    78, 78,                             /* switches */
    78, 78, 60, 32, 78, 96,             /* itemup wpnup getpow telept noway oof */
    96, 32, 32,                         /* plpain pldeth slop */
    98, 98, 98, 98, 94,                 /* sights */
    96, 96,                             /* pains */
    70, 70, 70, 70, 32, 70,             /* deaths */
    70, 64, 70,                         /* sgtatk claw sklatk */
};

/* by class from C_ZOMBIE: sight, pain, death, melee */
static const u8 says[][4] = {
    [C_ZOMBIE - C_ZOMBIE] = {SFX_POSIT1, SFX_POPAIN, SFX_PODTH1, 0},
    [C_SERGEANT - C_ZOMBIE] = {SFX_POSIT1, SFX_POPAIN, SFX_PODTH1, 0},
    [C_IMP - C_ZOMBIE] = {SFX_BGSIT1, SFX_DMPAIN, SFX_BGDTH1, SFX_CLAW},
    [C_DEMON - C_ZOMBIE] = {SFX_SGTSIT, SFX_DMPAIN, SFX_SGTDTH, SFX_SGTATK},
    [C_SPECTRE - C_ZOMBIE] = {SFX_SGTSIT, SFX_DMPAIN, SFX_SGTDTH, SFX_SGTATK},
    [C_CACODEMON - C_ZOMBIE] = {SFX_CACSIT, SFX_DMPAIN, SFX_CACDTH, 0},
    [C_BARON - C_ZOMBIE] = {SFX_BRSSIT, SFX_DMPAIN, SFX_BRSDTH, SFX_CLAW},
    [C_LOSTSOUL - C_ZOMBIE] = {0, SFX_DMPAIN, SFX_FIRXPL, SFX_SKLATK},
    [C_BARREL - C_ZOMBIE] = {0, 0, SFX_BAREXP, 0},
};

static u8 cur;                  /* the sound playing, 0 none */
static u32 cur_start;           /* hw_clock when it started */
static u8 cur_tone;             /* the tone the speaker has, 0 silent */

static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

void sound_load(void)
{
    u8 head[10];
    u16 n, size, i;
    u8 *p;
    if (tones) return;
    if (!hw_file_read("doom_snd.dat", 0, head, sizeof head)
        || head[0] != 'C' || head[1] != 'Y' || head[2] != 'S' || head[3] != 'N' || rd16(head + 4) != 1)
        return;
    n = rd16(head + 6);
    size = rd16(head + 8);
    if (n > NUMSFX) n = NUMSFX;
    p = hw_alloc((u32)rd16(head + 6) * 4 + size);
    if (!p || !hw_file_read("doom_snd.dat", sizeof head, p, (u32)rd16(head + 6) * 4 + size)) return;
    for (i = 0; i < n; i++) {
        sfx_off[i] = rd16(p + 4 * i);
        sfx_len[i] = rd16(p + 4 * i + 2);
        if ((u32)sfx_off[i] + sfx_len[i] > size) sfx_len[i] = 0;
    }
    tones = p + 4 * rd16(head + 6);
}

int sound_loaded(void)
{
    return tones != NULL;
}

static void set_tone(u8 t)
{
    if (t == cur_tone) return;
    cur_tone = t;
    hw_tone(t < 128 ? divisors[t] : 0);
}

void sound_stop(void)
{
    cur = 0;
    set_tone(0);
}

void sound_start(int sfx)
{
    if (!sound_on || !tones || sfx <= 0 || sfx >= NUMSFX || !sfx_len[sfx]) return;
    if (cur && priority[sfx] > priority[cur]) return;
    cur = (u8)sfx;
    cur_start = hw_clock();
    cur_tone = 0xff;            /* set the first tone even if it is the last one */
    sound_update();
}

void sound_at(const struct mobj_s *mo, int sfx)
{
    if (mo && player.mo && mo != player.mo
        && approx_dist((mo->x - player.mo->x) >> FRACBITS, (mo->y - player.mo->y) >> FRACBITS) > 1200)
        return;
    sound_start(sfx);
}

void sound_say(const struct mobj_s *mo, int what)
{
    if (mo->cls >= C_ZOMBIE && mo->cls <= C_BARREL) sound_at(mo, says[mo->cls - C_ZOMBIE][what]);
}

void sound_update(void)
{
    u32 pos;
    if (!cur) return;
    pos = (hw_clock() - cur_start) * 140 / HW_HZ;
    if (pos >= sfx_len[cur]) {
        sound_stop();
        return;
    }
    set_tone(tones[sfx_off[cur] + pos]);
}
