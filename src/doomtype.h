/* Basic types and fixed-point helpers. The same code builds for the Cybiko
 * (H8S, gcc 3.4 -ms: int is 16 bits, long 32) and for the PC test harness
 * (pc/), where long is 64 bits, hence the explicit types. */
#ifndef DOOMTYPE_H
#define DOOMTYPE_H

typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef unsigned short u16;
#ifdef __H8300S__
typedef long s32;
typedef unsigned long u32;
#else
typedef int s32;
typedef unsigned int u32;
#endif

#define NULL ((void *)0)
#define FRACBITS 16
#define FRACUNIT ((s32)1 << FRACBITS)

/* angles: 0-65535 is a full turn (16-bit BAM), 0 = east, counter-clockwise */
typedef u16 angle_t;
#define ANG90 0x4000
#define ANG180 0x8000
#define ANG45 0x2000

/* sine and cosine in Q14, 1024 steps a turn */
extern const s16 finesine[1024 + 256];
#define FINESHIFT 6
#define fsin(a) (finesine[(u16)(a) >> FINESHIFT])
#define fcos(a) (finesine[((u16)(a) >> FINESHIFT) + 256])

/* 16 x 16 -> 32 signed (one mulxs.w on the H8S) */
#define MUL16(a, b) ((s32)(s16)(a) * (s32)(s16)(b))

/* n / d where the quotient fits in 16 bits (one divxs.w); d != 0 */
s16 div32_16(s32 n, s16 d);
/* n / d with a 32-bit quotient; d != 0 */
s32 ldiv32_16(s32 n, s16 d);
/* (a * b) >> 16 for 16.16 fixed point */
s32 fixmul(s32 a, s32 b);
/* (a << 16) / b, clamped */
s32 fixdiv(s32 a, s32 b);

angle_t point_to_angle(s32 dx, s32 dy);   /* any units */
u16 approx_dist(s32 dx, s32 dy);          /* map units, saturates */

#define ABS(x) ((x) < 0 ? -(x) : (x))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void mset(void *d, u8 c, u32 n);
void mcpy(void *d, const void *s, u32 n);

u8 prandom(void);          /* 0-255, Doom's table */

#endif
