/* The map in memory, unpacked from the level pack that tools/wad2cy.py
 * makes from the WAD. */
#ifndef LEVEL_H
#define LEVEL_H
#include "doomtype.h"

struct mobj_s;

typedef struct {
    s16 x, y;
} vertex_t;

typedef struct sector_s {
    s16 floorh, ceilh;          /* map units; doors and lifts move them */
    u8 light;                   /* 0-255 */
    u8 special;
    s16 tag;
    u8 floortone, ceiltone;     /* brightness of the flats, ceiltone 0 = sky */
    u8 liquid;                  /* nukage, water, lava: drawn darker */
    u8 soundtraversed;          /* monsters hear the player through here */
    struct mobj_s *soundtarget;
    void *mover;                /* the door/lift/floor moving it, if any */
    s16 nlines;                 /* lines touching the sector: */
    s16 *lines;                 /* indices into lines[] */
    u16 validcount;
    s16 orig_floor, orig_ceil;  /* for "lowest ceiling" and similar searches */
} sector_t;

typedef struct {
    u8 top, bottom, mid;        /* texture tones, 0 = no texture */
    u8 ttop, tbottom, tmid;     /* textures (lvl.tex), NOTEX = none */
    s16 yoffset;
    s16 sector;
} side_t;

#define NOTEX 255

/* a wall texture, shrunk (wad2cy.py --texels), in 16 tones */
typedef struct {
    const u8 *texels;           /* column by column */
    u8 wbits, hbits;            /* log2 of the texel counts */
    u8 shift;                   /* log2 of the pixels (map units) per texel */
} texture_t;

#define ML_BLOCKING 1
#define ML_BLOCKMONSTERS 2
#define ML_TWOSIDED 4
#define ML_DONTPEGTOP 8
#define ML_DONTPEGBOTTOM 16
#define ML_SECRET 32
#define ML_SOUNDBLOCK 64
#define ML_DONTDRAW 128
#define ML_MAPPED 256

typedef struct {
    s16 v1, v2;
    u16 flags;
    s16 special, tag;
    s16 side[2];                /* -1 if none */
    s16 sec[2];                 /* front and back sector, -1 if none */
    s16 dx, dy;
    s16 bbox[4];                /* top, bottom, left, right as in Doom */
    u16 validcount;
} line_t;

#define BOXTOP 0
#define BOXBOTTOM 1
#define BOXLEFT 2
#define BOXRIGHT 3

typedef struct {
    s16 v1, v2;
    s16 line;
    u8 side;                    /* 0 = front of the line, 1 = back */
    u8 edge;                    /* 1: starts at the line's start, 2: ends at its end */
    s16 front, back;            /* sectors, back -1 for one-sided */
    u8 top, bottom, mid;        /* tones of this side's textures */
    u8 ttop, tbottom, tmid;     /* and the textures */
    s16 u;                      /* texture column (units) at v1 */
    s16 length;                 /* units */
    s16 yoffset;                /* the side's */
} seg_t;

typedef struct {
    s16 numsegs, firstseg;
    s16 sector;
    struct mobj_s *things;      /* mobjs whose centre is here */
} subsector_t;

#define NF_SUBSECTOR 0x8000

typedef struct {
    s16 x, y, dx, dy;
    s16 bbox[2][4];
    u16 children[2];
} node_t;

typedef struct {
    s16 x, y, angle, cls, flags;
} mapthing_t;

typedef struct {
    char name[5];
    s16 nvertexes, nsectors, nsides, nlines, nsegs, nsubsectors, nnodes, nthings;
    vertex_t *vertexes;
    sector_t *sectors;
    side_t *sides;
    line_t *lines;
    seg_t *segs;
    subsector_t *subsectors;
    node_t *nodes;
    mapthing_t *things;
    /* blockmap: origin in map units, size in 128-unit cells */
    s16 bmorgx, bmorgy, bmwidth, bmheight;
    s16 *blockmap;              /* the lump (offsets, then line lists) */
    struct mobj_s **blocklinks; /* mobjs per cell */
    const u8 *reject;
    s16 nreject;
    texture_t *tex;
    s16 ntex;
} level_t;

extern level_t lvl;
extern u16 validcount;

int pack_open(void);                    /* 0 if the pack cannot be read */
int level_count(void);
const char *level_name(int i);
int level_find(const char *name);       /* -1 if not in the pack */
int level_load(int i);                  /* 0 on failure */

/* the shrunk sprite for a class and frame (see wad2cy.py), or NULL */
const u8 *sprite_get(int cls, int frame);

subsector_t *point_in_subsector(s32 x, s32 y);     /* 16.16 */
int point_on_line_side(s32 x, s32 y, const line_t *l);  /* 16.16; 0 front */
int box_on_line_side(const s32 *box, const line_t *l);  /* -1 if crossing */
sector_t *other_sector(const line_t *l, const sector_t *s);

#endif
