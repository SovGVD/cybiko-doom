/* Fixed-point helpers. On the H8S the divisions are in rt.s (divxs.w). */
#include "doomtype.h"

extern const u16 tantoangle[257];

#ifndef __H8300S__
s16 div32_16(s32 n, s16 d)
{
    return (s16)(n / d);
}

s32 ldiv32_16(s32 n, s16 d)
{
    return n / d;
}

void mset(void *d, u8 c, u32 n)
{
    u8 *p = d;
    while (n--) *p++ = c;
}

void mcpy(void *d, const void *s, u32 n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--) *p++ = *q++;
}
#endif

/* (a * b) >> 16 */
s32 fixmul(s32 a, s32 b)
{
    int neg = 0;
    u32 ua, ub, r;
    if (a < 0) { a = -a; neg ^= 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    ua = (u32)a;
    ub = (u32)b;
    r = ((ua >> 16) * (ub >> 16) << 16)
        + (ua >> 16) * (ub & 0xffff) + (ua & 0xffff) * (ub >> 16)
        + (((ua & 0xffff) * (ub & 0xffff)) >> 16);
    return neg ? -(s32)r : (s32)r;
}

/* (a << 16) / b, clamped to about +-32767 */
s32 fixdiv(s32 a, s32 b)
{
    int neg = 0, k;
    u32 ua, ub, q = 0, r = 0;
    if (!b) return a < 0 ? -0x7fffffffL : 0x7fffffffL;
    if (a < 0) { a = -a; neg ^= 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    ua = (u32)a;
    ub = (u32)b;
    if ((ua >> 15) >= ub) return neg ? -0x7fffffffL : 0x7fffffffL;
    /* long division of ua << 16 by ub */
    for (k = 47; k >= 0; k--) {
        r = (r << 1) | (k >= 16 ? (ua >> (k - 16)) & 1 : 0);
        q <<= 1;
        if (r >= ub) {
            r -= ub;
            q |= 1;
        }
    }
    return neg ? -(s32)q : (s32)q;
}

angle_t point_to_angle(s32 dx, s32 dy)
{
    s32 ax = ABS(dx), ay = ABS(dy);
    u16 t;
    if (!ax && !ay) return 0;
    while (ax > 0x7fff || ay > 0x7fff) {
        ax >>= 1;
        ay >>= 1;
    }
    if (ax >= ay) t = tantoangle[div32_16(ay << 8, (s16)ax)];
    else t = (u16)(ANG90 - tantoangle[div32_16(ax << 8, (s16)ay)]);
    /* t is 0..90 degrees in the first quadrant */
    if (dx >= 0) return (angle_t)(dy >= 0 ? t : (u16)(0 - t));
    return (angle_t)(dy >= 0 ? (u16)(ANG180 - t) : (u16)(ANG180 + t));
}

u16 approx_dist(s32 dx, s32 dy)
{
    s32 d;
    dx = ABS(dx);
    dy = ABS(dy);
    d = dx + dy - ((dx < dy ? dx : dy) >> 1);
    return d > 0xffff ? 0xffff : (u16)d;
}
